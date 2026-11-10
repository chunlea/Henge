#include "pflash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What EDK2's VirtNorFlashDxe sends, each command in the low byte of the word:
   0xff read array, 0x70 read status, 0x50 clear status, 0x90 read identifiers,
   0x98 CFI query, 0x60 then 0xd0 or 0x01 unlock or lock a block, 0x20 then 0xd0
   erase a block, 0x40 or 0x10 then the word program it, 0xe8 then the word count
   less one, the words and 0xd0 program them from the buffer. Everything finishes
   at once, so the status is always "ready" (bit 7). */
enum { M_ARRAY = 0, M_STATUS, M_ID, M_LOCK, M_ERASE, M_PROGRAM, M_BUF_COUNT, M_BUF_DATA, M_BUF_CONFIRM };

static void save(HvPflash *f) {
    if (!f->dirty || !f->path) return;
    FILE *o = fopen(f->path, "r+b");
    if (!o) o = fopen(f->path, "wb");
    if (!o) {
        fprintf(stderr, "hengevm: cannot write the firmware's variables to %s\n", f->path);
        return;
    }
    fwrite(f->data, 1, HV_PFLASH_KEPT, o);
    fclose(o);
    f->dirty = 0;
}

static uint64_t pflash_read(void *opaque, uint64_t off, unsigned size) {
    HvPflash *f = opaque;
    if (f->mode == M_ARRAY) {
        uint64_t v = ~0ull;
        if (off + size <= HV_PFLASH_KEPT) memcpy(&v, f->data + off, size);
        return v;
    }
    if (f->mode == M_ID) return 0; /* no identifiers, no block locked */
    return f->status * 0x00010001u; /* a 32-bit bank of two 16-bit chips, as QEMU's virt machine has it */
}

/* Programming can only clear bits. */
static void program(HvPflash *f, uint64_t off, uint64_t v, unsigned size) {
    if (off + size > HV_PFLASH_KEPT) return;
    for (unsigned i = 0; i < size; i++) f->data[off + i] &= (uint8_t)(v >> (i * 8));
    f->dirty = 1;
}

static void pflash_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPflash *f = opaque;
    uint8_t cmd = (uint8_t)v;
    switch (f->mode) {
    case M_PROGRAM:
        program(f, off, v, size);
        f->mode = M_STATUS;
        save(f);
        return;
    case M_BUF_COUNT:
        f->left = ((uint32_t)v & 0xffff) + 1;
        f->mode = M_BUF_DATA;
        return;
    case M_BUF_DATA:
        program(f, off, v, size);
        if (--f->left == 0) f->mode = M_BUF_CONFIRM;
        return;
    case M_BUF_CONFIRM:
        f->mode = M_STATUS;
        if (cmd == 0xd0) save(f);
        else f->status |= 0x30; /* a command sequence error */
        return;
    case M_ERASE:
        f->mode = M_STATUS;
        if (cmd == 0xd0) {
            uint64_t base = off & ~(uint64_t)(HV_PFLASH_BLOCK - 1);
            if (base + HV_PFLASH_BLOCK <= HV_PFLASH_KEPT) {
                memset(f->data + base, 0xff, HV_PFLASH_BLOCK);
                f->dirty = 1;
                save(f);
            }
        } else f->status |= 0x30;
        return;
    case M_LOCK:
        f->mode = M_STATUS; /* 0xd0 unlock, 0x01 lock: nothing is ever locked */
        return;
    default: break;
    }
    switch (cmd) {
    case 0xff: f->mode = M_ARRAY; break;
    case 0x70: f->mode = M_STATUS; break;
    case 0x50: f->status = 0x80; f->mode = M_ARRAY; break;
    case 0x90: case 0x98: f->mode = M_ID; break;
    case 0x60: f->mode = M_LOCK; break;
    case 0x20: case 0x28: f->mode = M_ERASE; break;
    case 0x10: case 0x40: f->mode = M_PROGRAM; break;
    case 0xe8: f->mode = M_BUF_COUNT; break;
    default: f->mode = M_ARRAY; break;
    }
}

HvPflash *hv_pflash_create(HvMachine *m, uint64_t pa, uint64_t size, const char *path) {
    HvPflash *f = calloc(1, sizeof *f);
    f->data = malloc(HV_PFLASH_KEPT);
    if (!f->data) { free(f); return NULL; }
    memset(f->data, 0xff, HV_PFLASH_KEPT);
    f->size = size;
    f->status = 0x80;
    if (path) {
        f->path = strdup(path);
        FILE *in = fopen(path, "rb");
        if (in) {
            size_t n = fread(f->data, 1, HV_PFLASH_KEPT, in);
            (void)n;
            fclose(in);
        }
    }
    hv_machine_add_mmio(m, pa, size, f, pflash_read, pflash_write, "pflash");
    return f;
}

void hv_pflash_destroy(HvPflash *f) {
    if (!f) return;
    save(f);
    free(f->data);
    free(f->path);
    free(f);
}
