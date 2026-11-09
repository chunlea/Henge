// SPDX-License-Identifier: GPL-2.0-or-later
#include "linux_disk.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

typedef struct Fat {
    int fd;
    uint64_t part;        /* byte offset of the partition */
    uint32_t bps, spc;    /* bytes per sector, sectors per cluster */
    uint64_t fat_off, data_off;
    uint32_t root_cluster; /* FAT32; 0 for FAT12/16's fixed root */
    uint64_t root_off;     /* FAT12/16 */
    uint32_t root_entries;
    uint32_t nclusters;
    int bits;              /* 12, 16 or 32 */
} Fat;

static int rd(const Fat *f, uint64_t off, void *buf, size_t len) {
    return pread(f->fd, buf, len, (off_t)(f->part + off)) == (ssize_t)len ? 0 : -1;
}

static uint32_t next_cluster(const Fat *f, uint32_t c) {
    uint8_t b[4] = {0};
    if (f->bits == 32) {
        if (rd(f, f->fat_off + (uint64_t)c * 4, b, 4)) return 0x0fffffff;
        return le32(b) & 0x0fffffff;
    }
    if (f->bits == 16) {
        if (rd(f, f->fat_off + (uint64_t)c * 2, b, 2)) return 0x0fffffff;
        uint32_t v = le16(b);
        return v >= 0xfff8 ? 0x0fffffff : v;
    }
    if (rd(f, f->fat_off + c + c / 2, b, 2)) return 0x0fffffff;
    uint32_t v = le16(b);
    v = c & 1 ? v >> 4 : v & 0xfff;
    return v >= 0xff8 ? 0x0fffffff : v;
}

static int cluster_ok(const Fat *f, uint32_t c) { return c >= 2 && c < f->nclusters + 2; }
static uint64_t cluster_off(const Fat *f, uint32_t c) { return f->data_off + (uint64_t)(c - 2) * f->spc * f->bps; }

/* A file's (or directory's) contents by its cluster chain; size 0: until the chain ends. */
static uint8_t *read_chain(const Fat *f, uint32_t c, uint64_t size, size_t *len) {
    uint64_t csize = (uint64_t)f->spc * f->bps, cap = size ? size : csize, n = 0;
    uint8_t *buf = malloc(cap + 1);
    for (uint32_t steps = 0; buf && cluster_ok(f, c) && (!size || n < size); steps++) {
        if (steps > f->nclusters) { free(buf); return NULL; } /* a loop */
        uint64_t want = size ? (size - n < csize ? size - n : csize) : csize;
        if (n + want > cap) {
            cap = cap * 2 + csize;
            uint8_t *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        if (rd(f, cluster_off(f, c), buf + n, want)) { free(buf); return NULL; }
        n += want;
        c = next_cluster(f, c);
    }
    if (!buf || (size && n < size)) { free(buf); return NULL; }
    buf[n] = 0;
    *len = n;
    return buf;
}

typedef struct Ent {
    uint32_t cluster;
    uint32_t size;
    int dir;
} Ent;

/* Looks name up (case-insensitively, long or short name) in a directory's entries. */
static int dir_find(const uint8_t *d, size_t len, const char *name, Ent *out) {
    char lfn[256];
    int lfn_ok = 0;
    for (size_t at = 0; at + 32 <= len; at += 32) {
        const uint8_t *e = d + at;
        if (e[0] == 0) break;
        if (e[0] == 0xe5) { lfn_ok = 0; continue; }
        if (e[11] == 0x0f) { /* a long-name piece: 13 UTF-16 units, the last piece first */
            int seq = e[0] & 0x1f;
            if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_ok = 1; }
            static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int i = 0; i < 13 && seq >= 1; i++) {
                uint16_t ch = le16(e + pos[i]);
                int idx = (seq - 1) * 13 + i;
                if (idx < 255 && ch != 0xffff && ch) lfn[idx] = ch < 128 ? (char)ch : '?';
            }
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; continue; } /* the volume label */
        char shortn[13];
        int k = 0;
        for (int i = 0; i < 8 && e[i] != ' '; i++) shortn[k++] = (char)e[i];
        if (e[8] != ' ') {
            shortn[k++] = '.';
            for (int i = 8; i < 11 && e[i] != ' '; i++) shortn[k++] = (char)e[i];
        }
        shortn[k] = 0;
        int match = (lfn_ok && !strcasecmp(lfn, name)) || !strcasecmp(shortn, name);
        lfn_ok = 0;
        if (match) {
            out->cluster = (uint32_t)le16(e + 20) << 16 | le16(e + 26);
            out->size = le32(e + 28);
            out->dir = (e[11] & 0x10) != 0;
            return 0;
        }
    }
    return -1;
}

static uint8_t *fat_read(const Fat *f, const char *path, size_t *len) {
    uint8_t *dir;
    size_t dlen;
    if (f->bits == 32) dir = read_chain(f, f->root_cluster, 0, &dlen);
    else {
        dlen = (size_t)f->root_entries * 32;
        dir = malloc(dlen + 1);
        if (dir && rd(f, f->root_off, dir, dlen)) { free(dir); dir = NULL; }
    }
    char part[256];
    while (dir) {
        while (*path == '/') path++;
        size_t n = strcspn(path, "/");
        if (!n || n >= sizeof part) { free(dir); return NULL; }
        memcpy(part, path, n);
        part[n] = 0;
        path += n;
        Ent e;
        int found = dir_find(dir, dlen, part, &e);
        free(dir);
        if (found) return NULL;
        while (*path == '/') path++;
        if (!*path) {
            if (e.dir) return NULL;
            if (!e.size) { uint8_t *z = calloc(1, 1); if (z) *len = 0; return z; }
            return read_chain(f, e.cluster, e.size, len);
        }
        if (!e.dir) return NULL;
        dir = read_chain(f, e.cluster, 0, &dlen);
    }
    return NULL;
}

/* The ESP (or the first FAT partition) of a GPT disk, or a bare FAT image at offset 0. */
static int fat_open(Fat *f, const char *disk) {
    memset(f, 0, sizeof *f);
    f->fd = open(disk, O_RDONLY);
    if (f->fd < 0) return -1;
    static const uint8_t esp_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                         0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
    uint8_t h[92];
    uint64_t cand[8];
    int ncand = 0;
    if (pread(f->fd, h, sizeof h, 512) == (ssize_t)sizeof h && !memcmp(h, "EFI PART", 8)) {
        uint64_t start = le64(h + 72);
        uint32_t n = le32(h + 80), esz = le32(h + 84);
        if (esz < 128 || esz > 4096 || n > 1024) n = 0;
        uint8_t e[4096];
        for (uint32_t i = 0; i < n && ncand < 8; i++) {
            if (pread(f->fd, e, esz, (off_t)(start * 512 + (uint64_t)i * esz)) != (ssize_t)esz) break;
            if (!memcmp(e, esp_type, 16)) cand[ncand++] = le64(e + 32) * 512;
        }
    }
    cand[ncand++] = 0;
    for (int i = 0; i < ncand; i++) {
        uint8_t bs[512];
        if (pread(f->fd, bs, 512, (off_t)cand[i]) != 512 || bs[510] != 0x55 || bs[511] != 0xaa) continue;
        uint32_t bps = le16(bs + 11), spc = bs[13], rsv = le16(bs + 14), nfats = bs[16], rootent = le16(bs + 17);
        uint32_t tot = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
        uint32_t fatsz = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
        if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || !nfats || !fatsz) continue;
        uint32_t root_secs = (rootent * 32 + bps - 1) / bps;
        uint32_t data_sec = rsv + nfats * fatsz + root_secs;
        if (tot <= data_sec) continue;
        f->part = cand[i];
        f->bps = bps, f->spc = spc;
        f->fat_off = (uint64_t)rsv * bps;
        f->root_off = (uint64_t)(rsv + nfats * fatsz) * bps;
        f->root_entries = rootent;
        f->data_off = (uint64_t)data_sec * bps;
        f->nclusters = (tot - data_sec) / spc;
        f->bits = f->nclusters < 4085 ? 12 : f->nclusters < 65525 ? 16 : 32;
        f->root_cluster = f->bits == 32 ? le32(bs + 44) : 0;
        return 0;
    }
    close(f->fd);
    return -1;
}

uint8_t *hv_linux_disk_read_esp(const char *disk_path, const char *path, size_t *len) {
    Fat f;
    if (fat_open(&f, disk_path)) return NULL;
    uint8_t *r = fat_read(&f, path, len);
    close(f.fd);
    return r;
}

/* "boot():/a/b#hash" or "boot(1):/a/b" -> "/a/b". */
static char *limine_path(const char *v) {
    const char *p = strstr(v, "):");
    p = p ? p + 2 : v;
    size_t n = strcspn(p, "#\r\n");
    while (n && isspace((unsigned char)p[n - 1])) n--;
    char *r = malloc(n + 1);
    if (r) memcpy(r, p, n), r[n] = 0;
    return r;
}

static char *trim_dup(const char *v) {
    while (*v == ' ' || *v == '\t') v++;
    size_t n = strcspn(v, "\r\n");
    while (n && isspace((unsigned char)v[n - 1])) n--;
    char *r = malloc(n + 1);
    if (r) memcpy(r, v, n), r[n] = 0;
    return r;
}

/* The Linux entries of limine.conf, in order: the default_entry'th entry (counting
   every entry and directory, from 1) if that is one, otherwise the first. */
typedef struct LimEntry { char *k, *i, *c; int linux; } LimEntry;

static void lim_free(LimEntry *e) { free(e->k), free(e->i), free(e->c); memset(e, 0, sizeof *e); }

static int limine_entry(const char *conf, char **kernel, char **initrd, char **cmdline) {
    long want = 0;
    const char *de = strstr(conf, "default_entry:");
    if (de) want = strtol(de + 14, NULL, 10);
    LimEntry cur = {0}, first = {0}, chosen = {0};
    int entry = 0, have_first = 0, have_chosen = 0;
    const char *line = conf;
    for (int done = 0; !done;) {
        size_t len = strcspn(line, "\n");
        done = !line[len];
        char buf[4096];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        line += len + !done;
        char *s = buf;
        while (*s == ' ' || *s == '\t') s++;
        int is_entry = *s == '/';
        if (is_entry) {
            if (cur.linux && cur.k) {
                if (entry == want && !have_chosen) chosen = cur, have_chosen = 1, memset(&cur, 0, sizeof cur);
                else if (!have_first) first = cur, have_first = 1, memset(&cur, 0, sizeof cur);
            }
            lim_free(&cur);
            entry++;
            continue;
        }
        char *colon = strchr(s, ':');
        if (colon) {
            if (!strncasecmp(s, "protocol:", 9)) cur.linux = strstr(colon, "linux") != NULL;
            else if (!strncasecmp(s, "path:", 5) || !strncasecmp(s, "kernel_path:", 12)) free(cur.k), cur.k = limine_path(colon + 1);
            else if ((!strncasecmp(s, "module_path:", 12) || !strncasecmp(s, "initrd_path:", 12)) && !cur.i)
                cur.i = limine_path(colon + 1);
            else if (!strncasecmp(s, "cmdline:", 8) || !strncasecmp(s, "kernel_cmdline:", 15)) free(cur.c), cur.c = trim_dup(colon + 1);
        }
    }
    if (cur.linux && cur.k) {
        if (entry == want && !have_chosen) chosen = cur, have_chosen = 1, memset(&cur, 0, sizeof cur);
        else if (!have_first) first = cur, have_first = 1, memset(&cur, 0, sizeof cur);
    }
    lim_free(&cur);
    LimEntry *use = have_chosen ? &chosen : have_first ? &first : NULL;
    if (!use) return -1;
    *kernel = use->k, *initrd = use->i, *cmdline = use->c;
    if (use == &chosen) lim_free(&first);
    else lim_free(&chosen);
    return 0;
}

/* Linux's consoles for this board: Limine's hvc0 (virtio console) becomes the PL011. */
static char *fix_cmdline(const char *c) {
    size_t n = strlen(c);
    char *r = malloc(n + 64);
    if (!r) return NULL;
    r[0] = 0;
    const char *p = c;
    int had_serial = 0;
    while (*p) {
        while (*p == ' ') p++;
        size_t w = strcspn(p, " ");
        if (!w) break;
        if (w == 12 && !strncmp(p, "console=hvc0", 12)) { strcat(r, r[0] ? " console=ttyAMA0" : "console=ttyAMA0"); had_serial = 1; }
        else {
            if (w >= 15 && !strncmp(p, "console=ttyAMA0", 15)) had_serial = 1;
            if (r[0]) strcat(r, " ");
            strncat(r, p, w);
        }
        p += w;
    }
    if (!had_serial) strcat(r, r[0] ? " console=ttyAMA0" : "console=ttyAMA0");
    return r;
}

static const char *conf_paths[] = {"/limine.conf", "/boot/limine.conf", "/limine/limine.conf", "/EFI/limine/limine.conf",
                                   "/EFI/BOOT/limine.conf"};

int hv_linux_disk_boot(const char *disk_path, HvLinuxDiskBoot *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (err && errlen) err[0] = 0;
    Fat f;
    if (fat_open(&f, disk_path)) {
        snprintf(err, errlen, "The disk has no EFI system partition Henge can read.");
        return -1;
    }
    int r = -1;
    for (size_t p = 0; p < sizeof conf_paths / sizeof *conf_paths && r; p++) {
        size_t clen;
        char *conf = (char *)fat_read(&f, conf_paths[p], &clen);
        if (!conf) continue;
        char *k = NULL, *i = NULL, *c = NULL;
        if (limine_entry(conf, &k, &i, &c) == 0) {
            out->kernel = fat_read(&f, k, &out->kernel_len);
            if (i) out->initrd = fat_read(&f, i, &out->initrd_len);
            if (out->kernel && (!i || out->initrd)) {
                out->cmdline = fix_cmdline(c ? c : "");
                r = 0;
            } else {
                snprintf(err, errlen, "limine.conf names %s, which is not on the EFI partition.", out->kernel ? i : k);
                hv_linux_disk_boot_free(out);
            }
        }
        free(k), free(i), free(c), free(conf);
        if (r == 0) break;
    }
    close(f.fd);
    if (r) {
        if (!err[0]) snprintf(err, errlen, "No Linux boot entry was found on the disk (Henge reads Limine's limine.conf).");
    }
    return r;
}

void hv_linux_disk_boot_free(HvLinuxDiskBoot *b) {
    free(b->kernel), free(b->initrd), free(b->cmdline);
    memset(b, 0, sizeof *b);
}
