// SPDX-License-Identifier: GPL-2.0-or-later
#include "acpi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timer.h"

typedef struct Buf {
    uint8_t *p;
    size_t n, cap;
    int failed;
} Buf;

static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 1024) + n;
        uint8_t *np = realloc(b->p, cap);
        if (!np) { b->failed = 1; return; }
        b->p = np, b->cap = cap;
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
}

static void u8(Buf *b, uint8_t v) { put(b, &v, 1); }
static void u16(Buf *b, uint16_t v) { put(b, &v, 2); }
static void u32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void u64(Buf *b, uint64_t v) { put(b, &v, 8); }
static void set32(Buf *b, size_t at, uint32_t v) { if (!b->failed) memcpy(b->p + at, &v, 4); }
static void set16(Buf *b, size_t at, uint16_t v) { if (!b->failed) memcpy(b->p + at, &v, 2); }

/* ---- AML (ACPI 6.5, chapter 20): just the encodings the DSDT below uses ---- */

/* op bytes, then the body with its length in front (the length counts its own bytes). */
static void aml_pkg(Buf *out, const uint8_t *op, size_t oplen, Buf *body) {
    put(out, op, oplen);
    size_t n = body->n;
    if (n + 1 < 0x40) u8(out, (uint8_t)(n + 1));
    else if (n + 2 < 0x1000) u8(out, (uint8_t)(0x40 | ((n + 2) & 0xf))), u8(out, (uint8_t)((n + 2) >> 4));
    else u8(out, (uint8_t)(0x80 | ((n + 3) & 0xf))), u8(out, (uint8_t)((n + 3) >> 4)), u8(out, (uint8_t)((n + 3) >> 12));
    put(out, body->p, n);
    if (body->failed) out->failed = 1;
    free(body->p);
    *body = (Buf){0};
}

static void aml_int(Buf *b, uint64_t v) {
    if (v <= 1) u8(b, (uint8_t)v);
    else if (v <= 0xff) u8(b, 0x0a), u8(b, (uint8_t)v);
    else if (v <= 0xffff) u8(b, 0x0b), u16(b, (uint16_t)v);
    else if (v <= 0xffffffffu) u8(b, 0x0c), u32(b, (uint32_t)v);
    else u8(b, 0x0e), u64(b, v);
}

static void aml_name(Buf *b, const char *seg) { u8(b, 0x08), put(b, seg, 4); } /* Name (seg, what follows) */
static void aml_string(Buf *b, const char *s) { u8(b, 0x0d), put(b, s, strlen(s) + 1); }

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

/* EisaId ("PNP0A08"): three letters in five bits each, then four hexadecimal digits. */
static void aml_eisaid(Buf *b, const char *id) {
    unsigned c1 = (unsigned)(id[0] - 0x40) & 0x1f, c2 = (unsigned)(id[1] - 0x40) & 0x1f, c3 = (unsigned)(id[2] - 0x40) & 0x1f;
    uint8_t v[5] = {0x0c, (uint8_t)(c1 << 2 | c2 >> 3), (uint8_t)(c2 << 5 | c3), (uint8_t)(hexval(id[3]) << 4 | hexval(id[4])),
                    (uint8_t)(hexval(id[5]) << 4 | hexval(id[6]))};
    put(b, v, 5);
}

static void aml_device(Buf *out, const char *seg, Buf *body) {
    Buf in = {0};
    put(&in, seg, 4);
    put(&in, body->p, body->n);
    if (body->failed) in.failed = 1;
    free(body->p);
    *body = (Buf){0};
    aml_pkg(out, (const uint8_t[]){0x5b, 0x82}, 2, &in);
}

static void aml_method(Buf *out, const char *seg, int nargs, Buf *body) {
    Buf in = {0};
    put(&in, seg, 4);
    u8(&in, (uint8_t)nargs);
    put(&in, body->p, body->n);
    free(body->p);
    *body = (Buf){0};
    aml_pkg(out, (const uint8_t[]){0x14}, 1, &in);
}

/* Name (seg, ResourceTemplate () { res }): a buffer with the end tag added. */
static void aml_resources(Buf *out, const char *seg, Buf *res) {
    u8(res, 0x79), u8(res, 0);
    Buf in = {0};
    aml_int(&in, res->n);
    put(&in, res->p, res->n);
    free(res->p);
    *res = (Buf){0};
    aml_name(out, seg);
    aml_pkg(out, (const uint8_t[]){0x11}, 1, &in);
}

static void res_memory32(Buf *r, uint32_t base, uint32_t len) {
    u8(r, 0x86), u16(r, 9), u8(r, 1), u32(r, base), u32(r, len); /* Memory32Fixed (ReadWrite, ...) */
}

/* Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { gsi } */
static void res_interrupt(Buf *r, uint32_t gsi) {
    u8(r, 0x89), u16(r, 6), u8(r, 1), u8(r, 1), u32(r, gsi);
}

static void dsdt_body(Buf *d, const HvVmapple *b) {
    Buf sb = {0}, dev = {0}, res = {0}, in = {0};
    char seg[8];
    for (int i = 0; i < b->ncpus; i++) {
        aml_name(&dev, "_HID"), aml_string(&dev, "ACPI0007");
        aml_name(&dev, "_UID"), aml_int(&dev, (uint64_t)i);
        snprintf(seg, sizeof seg, "C%03X", i);
        aml_device(&sb, seg, &dev);
    }

    aml_name(&dev, "_HID"), aml_string(&dev, "ARMH0011");
    aml_name(&dev, "_UID"), aml_int(&dev, 0);
    res_memory32(&res, (uint32_t)VIRT_UART, 0x1000);
    res_interrupt(&res, 32 + VIRT_IRQ_UART);
    aml_resources(&dev, "_CRS", &res);
    aml_device(&sb, "COM0", &dev);

    /* One interrupt link per slot: slot s raises SPI VIRT_IRQ_PCIE + s (devices/pcie.c). A link
       device, as in QEMU's tables, is what says the interrupt is level-triggered and active high. */
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        aml_name(&dev, "_HID"), aml_eisaid(&dev, "PNP0C0F");
        aml_name(&dev, "_UID"), aml_int(&dev, (uint64_t)s);
        res_interrupt(&res, (uint32_t)(32 + VIRT_IRQ_PCIE + s));
        aml_resources(&dev, "_PRS", &res);
        res_interrupt(&res, (uint32_t)(32 + VIRT_IRQ_PCIE + s));
        aml_resources(&dev, "_CRS", &res);
        aml_method(&dev, "_SRS", 1, &in);
        snprintf(seg, sizeof seg, "L%03X", s);
        aml_device(&sb, seg, &dev);
    }

    aml_name(&dev, "_HID"), aml_eisaid(&dev, "PNP0A08");
    aml_name(&dev, "_CID"), aml_eisaid(&dev, "PNP0A03");
    aml_name(&dev, "_SEG"), aml_int(&dev, 0);
    aml_name(&dev, "_BBN"), aml_int(&dev, 0);
    aml_name(&dev, "_UID"), aml_int(&dev, 0);
    aml_name(&dev, "_CCA"), aml_int(&dev, 1);
    /* _PRT: {address (slot, any function), pin INTA, the link, 0} */
    Buf prt = {0};
    u8(&prt, HV_PCI_SLOTS);
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        u8(&in, 4);
        aml_int(&in, (uint64_t)s << 16 | 0xffff);
        aml_int(&in, 0);
        snprintf(seg, sizeof seg, "L%03X", s);
        put(&in, seg, 4);
        aml_int(&in, 0);
        aml_pkg(&prt, (const uint8_t[]){0x12}, 1, &in);
    }
    aml_name(&dev, "_PRT");
    aml_pkg(&dev, (const uint8_t[]){0x12}, 1, &prt);
    u8(&in, 0xa4), aml_int(&in, VIRT_PCIE_ECAM); /* Return (the ECAM base) */
    aml_method(&dev, "_CBA", 0, &in);
    uint16_t buses = (uint16_t)(VIRT_PCIE_ECAM_SIZE >> 20);
    u8(&res, 0x88), u16(&res, 0x0d), u8(&res, 2), u8(&res, 0x0c), u8(&res, 0); /* WordBusNumber, fixed ends */
    u16(&res, 0), u16(&res, 0), u16(&res, (uint16_t)(buses - 1)), u16(&res, 0), u16(&res, buses);
    u8(&res, 0x87), u16(&res, 0x17), u8(&res, 0), u8(&res, 0x0c), u8(&res, 1); /* DWordMemory, fixed ends, read-write */
    u32(&res, 0), u32(&res, (uint32_t)VIRT_PCIE_MMIO), u32(&res, (uint32_t)(VIRT_PCIE_MMIO + VIRT_PCIE_MMIO_SIZE - 1)), u32(&res, 0),
        u32(&res, (uint32_t)VIRT_PCIE_MMIO_SIZE);
    u8(&res, 0x8a), u16(&res, 0x2b), u8(&res, 0), u8(&res, 0x0c), u8(&res, 1); /* QWordMemory: the window above RAM */
    u64(&res, 0), u64(&res, VIRT_PCIE_MMIO64), u64(&res, VIRT_PCIE_MMIO64 + VIRT_PCIE_MMIO64_SIZE - 1), u64(&res, 0),
        u64(&res, VIRT_PCIE_MMIO64_SIZE);
    aml_resources(&dev, "_CRS", &res);
    /* _OSC: the system may have every PCIe feature it asks for (Return (Arg3)). */
    u8(&in, 0xa4), u8(&in, 0x6b);
    aml_method(&dev, "_OSC", 4, &in);
    /* The ECAM region is nobody else's. */
    Buf res0 = {0};
    aml_name(&res0, "_HID"), aml_eisaid(&res0, "PNP0C02");
    res_memory32(&res, (uint32_t)VIRT_PCIE_ECAM, (uint32_t)VIRT_PCIE_ECAM_SIZE);
    aml_resources(&res0, "_CRS", &res);
    aml_device(&dev, "RES0", &res0);
    aml_device(&sb, "PCI0", &dev);

    Buf scope = {0};
    put(&scope, "\\_SB_", 5);
    put(&scope, sb.p, sb.n);
    if (sb.failed) scope.failed = 1;
    free(sb.p);
    aml_pkg(d, (const uint8_t[]){0x10}, 1, &scope);
}

/* ---- the tables ---- */

static size_t table_begin(Buf *b, const char *sig, uint8_t rev) {
    while (b->n & 7) u8(b, 0);
    size_t at = b->n;
    put(b, sig, 4);
    u32(b, 0);          /* length: table_end */
    u8(b, rev);
    u8(b, 0);           /* checksum: the firmware, told to by the loader */
    put(b, "HENGE ", 6);
    put(b, "HENGEVM ", 8);
    u32(b, 1);
    put(b, "HNGE", 4);
    u32(b, 1);
    return at;
}

static void gas(Buf *b, uint8_t width, uint8_t access, uint64_t addr) {
    u8(b, 0), u8(b, width), u8(b, 0), u8(b, access), u64(b, addr); /* system memory */
}

/* The firmware's instructions: 128 bytes each. */
enum { CMD_ALLOCATE = 1, CMD_ADD_POINTER = 2, CMD_ADD_CHECKSUM = 3 };
#define TABLES "etc/acpi/tables"
#define RSDP "etc/acpi/rsdp"

static void cmd_allocate(Buf *l, const char *file, uint32_t align, uint8_t zone) {
    uint8_t e[128] = {0};
    e[0] = CMD_ALLOCATE;
    snprintf((char *)e + 4, 56, "%s", file);
    memcpy(e + 60, &align, 4);
    e[64] = zone;
    put(l, e, sizeof e);
}

/* The `size` bytes at offset of dest hold an offset into src: make them its address. */
static void cmd_pointer(Buf *l, const char *dest, const char *src, uint32_t offset, uint8_t size) {
    uint8_t e[128] = {0};
    e[0] = CMD_ADD_POINTER;
    snprintf((char *)e + 4, 56, "%s", dest);
    snprintf((char *)e + 60, 56, "%s", src);
    memcpy(e + 116, &offset, 4);
    e[120] = size;
    put(l, e, sizeof e);
}

static void cmd_checksum(Buf *l, const char *file, uint32_t offset, uint32_t start, uint32_t length) {
    uint8_t e[128] = {0};
    e[0] = CMD_ADD_CHECKSUM;
    snprintf((char *)e + 4, 56, "%s", file);
    memcpy(e + 60, &offset, 4);
    memcpy(e + 64, &start, 4);
    memcpy(e + 68, &length, 4);
    put(l, e, sizeof e);
}

int hv_acpi_install(HvVmapple *b) {
    Buf t = {0}, l = {0}, sums = {0};
    size_t at[8];
    int nt = 0;
    cmd_allocate(&l, RSDP, 16, 2);   /* anywhere below 4 GB will do for both */
    cmd_allocate(&l, TABLES, 64, 1);
#define END(at_) do { set32(&t, (at_) + 4, (uint32_t)(t.n - (at_))); cmd_checksum(&sums, TABLES, (uint32_t)(at_) + 9, (uint32_t)(at_), (uint32_t)(t.n - (at_))); } while (0)

    size_t dsdt = table_begin(&t, "DSDT", 2);
    dsdt_body(&t, b);
    END(dsdt);

    /* FADT, ACPI 6.3: hardware-reduced, and PSCI is there and is called with HVC. */
    size_t fadt = at[nt++] = table_begin(&t, "FACP", 6);
    put(&t, NULL, 276 - 36);
    set32(&t, fadt + 112, 1u << 20);
    set16(&t, fadt + 129, 3);
    t.p[fadt + 131] = 3;
    uint64_t dsdt_off = dsdt;
    memcpy(t.p + fadt + 140, &dsdt_off, 8);
    cmd_pointer(&l, TABLES, TABLES, (uint32_t)fadt + 140, 8);
    END(fadt);

    size_t madt = at[nt++] = table_begin(&t, "APIC", 4);
    u32(&t, 0), u32(&t, 0);
    for (int i = 0; i < b->ncpus; i++) { /* GICC: the processor, by its _UID and MPIDR */
        u8(&t, 0x0b), u8(&t, 80), u16(&t, 0);
        u32(&t, (uint32_t)i), u32(&t, (uint32_t)i), u32(&t, 1); /* interface number, _UID, enabled */
        u32(&t, 0), u32(&t, 0), u64(&t, 0);                     /* parking protocol, no PMU interrupt, parked address */
        u64(&t, 0), u64(&t, 0), u64(&t, 0), u32(&t, 0);         /* GICv2's base addresses, no maintenance interrupt */
        u64(&t, 0);                                             /* its redistributor: in the GICR entry below */
        u64(&t, (uint64_t)i);                                   /* MPIDR's affinity fields (hv_virt_reset_cpu) */
        u8(&t, 0), u8(&t, 0), u16(&t, 0);
    }
    u8(&t, 0x0c), u8(&t, 24), u16(&t, 0), u32(&t, 0), u64(&t, VIRT_GIC_DIST), u32(&t, 0), u8(&t, 3), u8(&t, 0), u16(&t, 0);
    u8(&t, 0x0e), u8(&t, 16), u16(&t, 0), u64(&t, VIRT_GIC_REDIST), u32(&t, 0x20000u * (uint32_t)b->ncpus);
    END(madt);

    /* GTDT: each timer's interrupt, level-triggered and active high; no memory-mapped timers. */
    size_t gtdt = at[nt++] = table_begin(&t, "GTDT", 2);
    u64(&t, ~0ull), u32(&t, 0);
    u32(&t, 29), u32(&t, 0);
    u32(&t, HV_PPI_PTIMER), u32(&t, 0);
    u32(&t, HV_PPI_VTIMER), u32(&t, 0);
    u32(&t, 26), u32(&t, 0);
    u64(&t, ~0ull), u32(&t, 0), u32(&t, 0);
    END(gtdt);

    size_t mcfg = at[nt++] = table_begin(&t, "MCFG", 1);
    u64(&t, 0);
    u64(&t, VIRT_PCIE_ECAM), u16(&t, 0), u8(&t, 0), u8(&t, (uint8_t)((VIRT_PCIE_ECAM_SIZE >> 20) - 1)), u32(&t, 0);
    END(mcfg);

    /* SPCR: the console is the PL011. */
    size_t spcr = at[nt++] = table_begin(&t, "SPCR", 2);
    u8(&t, 3), u8(&t, 0), u16(&t, 0);
    gas(&t, 32, 3, VIRT_UART);
    u8(&t, 8), u8(&t, 0), u32(&t, 32 + VIRT_IRQ_UART); /* a GIC interrupt */
    u8(&t, 3), u8(&t, 0), u8(&t, 1), u8(&t, 0), u8(&t, 0), u8(&t, 0); /* 9600 8N1 as QEMU says, no flow control, VT100 */
    u16(&t, 0xffff), u16(&t, 0xffff), u8(&t, 0), u8(&t, 0), u8(&t, 0), u32(&t, 0), u8(&t, 0), u32(&t, 0); /* not a PCI device */
    END(spcr);

    /* DBG2: the same port for a kernel debugger. */
    static const char ns[] = "\\_SB.COM0";
    size_t dbg2 = at[nt++] = table_begin(&t, "DBG2", 0);
    u32(&t, 44), u32(&t, 1);
    u8(&t, 0), u16(&t, (uint16_t)(38 + sizeof ns)), u8(&t, 1);
    u16(&t, sizeof ns), u16(&t, 38), u16(&t, 0), u16(&t, 0);
    u16(&t, 0x8000), u16(&t, 3), u16(&t, 0); /* serial, PL011 */
    u16(&t, 22), u16(&t, 34);
    gas(&t, 32, 3, VIRT_UART);
    u32(&t, 0x1000);
    put(&t, ns, sizeof ns);
    END(dbg2);

    size_t xsdt = table_begin(&t, "XSDT", 1);
    for (int i = 0; i < nt; i++) {
        cmd_pointer(&l, TABLES, TABLES, (uint32_t)t.n, 8);
        u64(&t, at[i]);
    }
    END(xsdt);

    uint8_t rsdp[36] = "RSD PTR ";
    memcpy(rsdp + 9, "HENGE ", 6);
    rsdp[15] = 2;
    rsdp[20] = 36;
    uint64_t xsdt_off = xsdt;
    memcpy(rsdp + 24, &xsdt_off, 8);
    cmd_pointer(&l, RSDP, TABLES, 24, 8);
    put(&l, sums.p, sums.n); /* checksums last: the addresses are in by then */
    cmd_checksum(&l, RSDP, 8, 0, 20);
    cmd_checksum(&l, RSDP, 32, 0, 36);

    int r = -1;
    if (!t.failed && !l.failed && !sums.failed &&
        hv_fw_cfg_add_file(b->fwcfg, RSDP, rsdp, sizeof rsdp, NULL, NULL) == 0 &&
        hv_fw_cfg_add_file(b->fwcfg, TABLES, t.p, (uint32_t)t.n, NULL, NULL) == 0 &&
        hv_fw_cfg_add_file(b->fwcfg, "etc/table-loader", l.p, (uint32_t)l.n, NULL, NULL) == 0)
        r = 0;
    const char *out = getenv("HV_ACPI_OUT"); /* debugging: the blob, for iasl */
    if (out && r == 0) {
        FILE *o = fopen(out, "wb");
        if (o) fwrite(t.p, 1, t.n, o), fclose(o);
    }
    free(t.p), free(l.p), free(sums.p);
    return r;
}
