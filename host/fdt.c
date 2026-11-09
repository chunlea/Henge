// SPDX-License-Identifier: GPL-2.0-or-later
#include "fdt.h"

#include <stdlib.h>
#include <string.h>

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_END 9u

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static int grow(uint8_t **buf, size_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    uint8_t *n = realloc(*buf, c);
    if (!n) return -1;
    *buf = n, *cap = c;
    return 0;
}

static void put(HvFdt *f, const void *data, size_t len) {
    if (f->failed) return;
    size_t padded = (len + 3) & ~(size_t)3;
    if (grow(&f->st, &f->st_cap, f->st_len + padded)) { f->failed = 1; return; }
    memcpy(f->st + f->st_len, data, len);
    memset(f->st + f->st_len + len, 0, padded - len);
    f->st_len += padded;
}

static void put32(HvFdt *f, uint32_t v) {
    uint8_t b[4];
    be32(b, v);
    put(f, b, 4);
}

/* Offset of name in the strings block, adding it the first time. */
static uint32_t string_off(HvFdt *f, const char *name) {
    size_t n = strlen(name) + 1;
    for (size_t at = 0; at < f->str_len; at += strlen(f->str + at) + 1)
        if (!strcmp(f->str + at, name)) return (uint32_t)at;
    if (grow((uint8_t **)&f->str, &f->str_cap, f->str_len + n)) { f->failed = 1; return 0; }
    memcpy(f->str + f->str_len, name, n);
    f->str_len += n;
    return (uint32_t)(f->str_len - n);
}

void hv_fdt_init(HvFdt *f) { memset(f, 0, sizeof *f); }

void hv_fdt_begin(HvFdt *f, const char *name) {
    put32(f, FDT_BEGIN_NODE);
    put(f, name, strlen(name) + 1);
    f->depth++;
}

void hv_fdt_end(HvFdt *f) {
    if (f->depth-- <= 0) f->failed = 1;
    put32(f, FDT_END_NODE);
}

void hv_fdt_prop(HvFdt *f, const char *name, const void *data, size_t len) {
    uint32_t off = string_off(f, name);
    put32(f, FDT_PROP);
    put32(f, (uint32_t)len);
    put32(f, off);
    if (len) put(f, data, len);
}

void hv_fdt_prop_empty(HvFdt *f, const char *name) { hv_fdt_prop(f, name, NULL, 0); }

void hv_fdt_prop_u32(HvFdt *f, const char *name, uint32_t v) { hv_fdt_prop_cells(f, name, &v, 1); }

void hv_fdt_prop_u64(HvFdt *f, const char *name, uint64_t v) {
    uint32_t c[2] = {(uint32_t)(v >> 32), (uint32_t)v};
    hv_fdt_prop_cells(f, name, c, 2);
}

void hv_fdt_prop_str(HvFdt *f, const char *name, const char *s) { hv_fdt_prop(f, name, s, strlen(s) + 1); }

void hv_fdt_prop_strs(HvFdt *f, const char *name, const char *s, size_t len) { hv_fdt_prop(f, name, s, len); }

void hv_fdt_prop_cells(HvFdt *f, const char *name, const uint32_t *cells, int n) {
    uint8_t *b = malloc((size_t)n * 4 + 1);
    if (!b) { f->failed = 1; return; }
    for (int i = 0; i < n; i++) be32(b + i * 4, cells[i]);
    hv_fdt_prop(f, name, b, (size_t)n * 4);
    free(b);
}

uint8_t *hv_fdt_finish(HvFdt *f, size_t *len) {
    put32(f, FDT_END);
    uint8_t *blob = NULL;
    if (!f->failed && f->depth == 0) {
        const size_t hdr = 40, rsv = 16; /* the header, then one empty reservation entry */
        size_t off_st = hdr + rsv, off_str = off_st + f->st_len, total = off_str + f->str_len;
        blob = calloc(1, total);
        if (blob) {
            be32(blob + 0, FDT_MAGIC);
            be32(blob + 4, (uint32_t)total);
            be32(blob + 8, (uint32_t)off_st);
            be32(blob + 12, (uint32_t)off_str);
            be32(blob + 16, (uint32_t)hdr);     /* off_mem_rsvmap */
            be32(blob + 20, 17);                /* version */
            be32(blob + 24, 16);                /* last compatible version */
            be32(blob + 28, 0);                 /* boot CPU */
            be32(blob + 32, (uint32_t)f->str_len);
            be32(blob + 36, (uint32_t)f->st_len);
            memcpy(blob + off_st, f->st, f->st_len);
            memcpy(blob + off_str, f->str, f->str_len);
            *len = total;
        }
    }
    free(f->st);
    free(f->str);
    memset(f, 0, sizeof *f);
    return blob;
}
