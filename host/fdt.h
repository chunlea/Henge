// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_FDT_H
#define HENGEVM_FDT_H

#include <stddef.h>
#include <stdint.h>

/* A flattened device tree (devicetree specification v0.4, version 17) built in
   one pass: nodes and properties in order, then hv_fdt_finish. Enough for the
   tree a Linux guest boots with (host/linux_boot.c). */
typedef struct HvFdt {
    uint8_t *st;  /* structure block */
    size_t st_len, st_cap;
    char *str;    /* strings block */
    size_t str_len, str_cap;
    int depth, failed;
} HvFdt;

void hv_fdt_init(HvFdt *f);
void hv_fdt_begin(HvFdt *f, const char *name);
void hv_fdt_end(HvFdt *f);
void hv_fdt_prop(HvFdt *f, const char *name, const void *data, size_t len);
void hv_fdt_prop_empty(HvFdt *f, const char *name);
void hv_fdt_prop_u32(HvFdt *f, const char *name, uint32_t v);
void hv_fdt_prop_u64(HvFdt *f, const char *name, uint64_t v);
void hv_fdt_prop_str(HvFdt *f, const char *name, const char *s);
/* A string list: the strings are given NUL-separated, len counts the last NUL. */
void hv_fdt_prop_strs(HvFdt *f, const char *name, const char *s, size_t len);
/* n 32-bit cells, big-endian in the tree. */
void hv_fdt_prop_cells(HvFdt *f, const char *name, const uint32_t *cells, int n);
/* The blob (malloc'd, *len set), or NULL if anything failed or a node is left open. Frees the builder. */
uint8_t *hv_fdt_finish(HvFdt *f, size_t *len);

#endif
