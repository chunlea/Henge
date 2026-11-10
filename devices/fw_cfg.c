#include "fw_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Item keys (QEMU's include/standard-headers/linux/qemu_fw_cfg.h). */
enum { KEY_SIGNATURE = 0, KEY_ID = 1, KEY_NB_CPUS = 5, KEY_FILE_DIR = 0x19, KEY_FILE_FIRST = 0x20 };
enum { DMA_ERROR = 1, DMA_READ = 2, DMA_SKIP = 4, DMA_SELECT = 8, DMA_WRITE = 16 };

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

/* The selected item's bytes, or NULL (an item nobody has: it reads as zeros). */
static const uint8_t *item(HvFwCfg *f, uint32_t *size, uint8_t scratch[4]) {
    switch (f->sel) {
    case KEY_SIGNATURE: *size = 4; return (const uint8_t *)"QEMU";
    case KEY_ID: /* the traditional interface and DMA */
        scratch[0] = 3, scratch[1] = scratch[2] = scratch[3] = 0;
        *size = 4;
        return scratch;
    case KEY_NB_CPUS:
        scratch[0] = (uint8_t)f->ncpus, scratch[1] = (uint8_t)(f->ncpus >> 8);
        *size = 2;
        return scratch;
    case KEY_FILE_DIR: *size = f->dir_size; return f->dir;
    default:
        if (f->sel >= KEY_FILE_FIRST && f->sel - KEY_FILE_FIRST < f->nfiles) {
            *size = f->file[f->sel - KEY_FILE_FIRST].size;
            return f->file[f->sel - KEY_FILE_FIRST].data;
        }
        *size = 0;
        return NULL;
    }
}

static uint8_t next_byte(HvFwCfg *f) {
    uint32_t size;
    uint8_t scratch[4];
    const uint8_t *p = item(f, &size, scratch);
    return p && f->off < size ? p[f->off++] : 0;
}

static void dma(HvFwCfg *f, uint64_t pa) {
    uint8_t req[16];
    if (hv_phys_read(f->m, pa, req, sizeof req) != 0) return;
    uint32_t control = be32(req), len = be32(req + 4), result = 0;
    uint64_t addr = (uint64_t)be32(req + 8) << 32 | be32(req + 12);
    if (control & DMA_SELECT) {
        f->sel = (uint16_t)(control >> 16), f->off = 0;
        if (f->sel == KEY_FILE_DIR && f->dir_selected) f->dir_selected(f->dir_opaque);
    }
    uint32_t size;
    uint8_t scratch[4];
    const uint8_t *p = item(f, &size, scratch);
    uint32_t have = p && f->off < size ? size - f->off : 0;
    if (control & DMA_READ) {
        uint32_t n = len < have ? len : have;
        uint8_t *to = len ? hv_phys_ptr(f->m, addr, len) : NULL;
        if (len && !to) result = DMA_ERROR;
        else if (len) {
            if (n) memcpy(to, p + f->off, n);
            memset(to + n, 0, len - n);
            hv_dma_written(f->m, addr, len);
        }
        f->off += n;
    } else if (control & DMA_WRITE) {
        HvFwCfgFile *file = f->sel >= KEY_FILE_FIRST && f->sel - KEY_FILE_FIRST < f->nfiles ? &f->file[f->sel - KEY_FILE_FIRST] : NULL;
        const uint8_t *from = len ? hv_phys_ptr(f->m, addr, len) : NULL;
        if (!file || !file->written || (len && !from)) result = DMA_ERROR;
        else {
            uint32_t n = len < have ? len : have;
            if (n) memcpy(file->data + f->off, from, n);
            f->off += n;
            file->written(file->opaque);
        }
    } else if (control & DMA_SKIP) {
        f->off += len < have ? len : have;
    }
    put_be32(req, result);
    hv_phys_write(f->m, pa, req, 4);
}

static uint64_t fw_cfg_read(void *opaque, uint64_t off, unsigned size) {
    HvFwCfg *f = opaque;
    if (off < 8) { /* the data register: the next `size` bytes, first byte lowest */
        uint64_t v = 0;
        for (unsigned i = 0; i < size && i < 8; i++) v |= (uint64_t)next_byte(f) << (i * 8);
        return v;
    }
    if (off == 8) return (uint16_t)(f->sel << 8 | f->sel >> 8);
    /* The DMA register reads as "QEMU CFG", big-endian. */
    static const uint8_t sig[8] = {'Q', 'E', 'M', 'U', ' ', 'C', 'F', 'G'};
    uint64_t v = 0;
    for (unsigned i = 0; i < size && off - 16 + i < 8; i++) v |= (uint64_t)sig[off - 16 + i] << (i * 8);
    return v;
}

static void fw_cfg_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvFwCfg *f = opaque;
    if (off == 8 && size == 2) {
        f->sel = (uint16_t)((v & 0xff) << 8 | ((v >> 8) & 0xff));
        if (f->sel == KEY_FILE_DIR && f->dir_selected) f->dir_selected(f->dir_opaque);
        f->off = 0;
    } else if (off == 16 && size == 8) {
        dma(f, __builtin_bswap64(v));
    } else if (off == 16 && size == 4) {
        f->dma_high = __builtin_bswap32((uint32_t)v);
    } else if (off == 20 && size == 4) {
        uint64_t pa = (uint64_t)f->dma_high << 32 | __builtin_bswap32((uint32_t)v);
        f->dma_high = 0;
        dma(f, pa);
    }
}

static void rebuild_dir(HvFwCfg *f) {
    free(f->dir);
    f->dir_size = 4 + 64u * (uint32_t)f->nfiles;
    f->dir = calloc(1, f->dir_size);
    put_be32(f->dir, (uint32_t)f->nfiles);
    for (int i = 0; i < f->nfiles; i++) {
        uint8_t *e = f->dir + 4 + 64 * i;
        put_be32(e, f->file[i].size);
        e[4] = (uint8_t)((KEY_FILE_FIRST + i) >> 8), e[5] = (uint8_t)(KEY_FILE_FIRST + i);
        memcpy(e + 8, f->file[i].name, 56);
    }
}

int hv_fw_cfg_add_file(HvFwCfg *f, const char *name, const void *data, uint32_t size, void (*written)(void *opaque),
                       void *opaque) {
    if (f->nfiles == HV_FWCFG_FILES) return -1;
    HvFwCfgFile *file = &f->file[f->nfiles];
    memset(file, 0, sizeof *file);
    snprintf(file->name, sizeof file->name, "%s", name);
    file->data = calloc(1, size ? size : 1);
    if (!file->data) return -1;
    if (data) memcpy(file->data, data, size);
    file->size = size;
    file->written = written;
    file->opaque = opaque;
    f->nfiles++;
    rebuild_dir(f);
    return 0;
}

HvFwCfgFile *hv_fw_cfg_find(HvFwCfg *f, const char *name) {
    for (int i = 0; i < f->nfiles; i++)
        if (!strcmp(f->file[i].name, name)) return &f->file[i];
    return NULL;
}

HvFwCfg *hv_fw_cfg_create(HvMachine *m, uint64_t pa, int ncpus) {
    HvFwCfg *f = calloc(1, sizeof *f);
    f->m = m;
    f->ncpus = (uint16_t)ncpus;
    rebuild_dir(f);
    hv_machine_add_mmio(m, pa, 0x18, f, fw_cfg_read, fw_cfg_write, "fw-cfg");
    return f;
}

void hv_fw_cfg_destroy(HvFwCfg *f) {
    if (!f) return;
    for (int i = 0; i < f->nfiles; i++) free(f->file[i].data);
    free(f->dir);
    free(f);
}
