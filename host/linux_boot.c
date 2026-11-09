// SPDX-License-Identifier: GPL-2.0-or-later
#include "linux_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "fdt.h"
#include "linux_disk.h"
#include "timer.h"

#define MB (1ull << 20)
#define LINUX_CONSOLE "console=ttyAMA0 earlycon=pl011,mmio32,0x20010000"

static int fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return -1;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 16 * MB;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    *len = n;
    return buf;
}

/* A gzip stream, inflated. */
static uint8_t *gunzip(const uint8_t *in, size_t inlen, size_t *outlen) {
    z_stream z = {0};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return NULL;
    size_t cap = inlen * 4 + MB, n = 0;
    uint8_t *out = malloc(cap);
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)inlen;
    int r = Z_OK;
    while (out && r != Z_STREAM_END) {
        if (n == cap) {
            cap *= 2;
            uint8_t *no = realloc(out, cap);
            if (!no) { free(out); out = NULL; break; }
            out = no;
        }
        z.next_out = out + n;
        z.avail_out = (uInt)(cap - n);
        r = inflate(&z, Z_NO_FLUSH);
        n = cap - z.avail_out;
        if (r != Z_OK && r != Z_STREAM_END) { free(out); out = NULL; }
    }
    inflateEnd(&z);
    if (out) *outlen = n;
    return out;
}

/* A PE section by name (unified kernel images), or NULL. */
static const uint8_t *pe_section(const uint8_t *img, size_t len, const char *name, size_t *slen) {
    if (len < 0x40 || img[0] != 'M' || img[1] != 'Z') return NULL;
    uint32_t pe = le32(img + 0x3c);
    if ((uint64_t)pe + 24 > len || memcmp(img + pe, "PE\0\0", 4)) return NULL;
    unsigned nsec = img[pe + 6] | img[pe + 7] << 8, opt = img[pe + 20] | img[pe + 21] << 8;
    uint64_t sec = (uint64_t)pe + 24 + opt;
    for (unsigned i = 0; i < nsec; i++, sec += 40) {
        if (sec + 40 > len) return NULL;
        char n[9] = {0};
        memcpy(n, img + sec, 8);
        if (strcmp(n, name)) continue;
        uint32_t vsize = le32(img + sec + 8), raw = le32(img + sec + 16), off = le32(img + sec + 20);
        uint32_t size = vsize && vsize < raw ? vsize : raw; /* raw data is padded to the file alignment */
        if ((uint64_t)off + size > len) return NULL;
        *slen = size;
        return img + off;
    }
    return NULL;
}

/* Down to a bare arm64 Image ("ARM\x64" at 56). Takes ownership of img. */
static uint8_t *to_image(uint8_t *img, size_t *len, int depth) {
    if (!img || depth > 4) { free(img); return NULL; }
    size_t n = *len, out_len = 0;
    uint8_t *out = NULL;
    if (n >= 64 && !memcmp(img + 4, "zimg", 4)) { /* EFI zboot: compressed payload at +8 (offset) / +12 (size) */
        uint32_t off = le32(img + 8), size = le32(img + 12);
        if ((uint64_t)off + size <= n && !strncmp((char *)img + 0x18, "gzip", 4)) out = gunzip(img + off, size, &out_len);
    } else if (n >= 64 && !memcmp(img + 56, "ARM\x64", 4)) {
        return img;
    } else if (n >= 2 && img[0] == 0x1f && img[1] == 0x8b) {
        out = gunzip(img, n, &out_len);
    } else {
        size_t sl;
        const uint8_t *s = pe_section(img, n, ".linux", &sl);
        if (s && (out = malloc(sl))) memcpy(out, s, sl), out_len = sl;
    }
    free(img);
    if (!out) return NULL;
    *len = out_len;
    return to_image(out, len, depth + 1);
}

static void reg2(uint32_t *c, uint64_t addr, uint64_t size) {
    c[0] = (uint32_t)(addr >> 32), c[1] = (uint32_t)addr, c[2] = (uint32_t)(size >> 32), c[3] = (uint32_t)size;
}

enum { PH_GIC = 1, PH_CLK = 2 };

unsigned char *hv_linux_make_dtb(const HvVmapple *b, const char *cmdline, unsigned long long initrd_start,
                                 unsigned long long initrd_end, size_t *len) {
    HvFdt f;
    uint32_t c[16 + HV_PCI_SLOTS * 7];
    hv_fdt_init(&f);
    hv_fdt_begin(&f, "");
    hv_fdt_prop_u32(&f, "#address-cells", 2);
    hv_fdt_prop_u32(&f, "#size-cells", 2);
    static const char compat[] = "henge,linux\0linux,dummy-virt";
    hv_fdt_prop_strs(&f, "compatible", compat, sizeof compat);
    hv_fdt_prop_str(&f, "model", "Henge");
    hv_fdt_prop_u32(&f, "interrupt-parent", PH_GIC);

    hv_fdt_begin(&f, "chosen");
    hv_fdt_prop_str(&f, "bootargs", cmdline);
    hv_fdt_prop_str(&f, "stdout-path", "/pl011@20010000");
    if (initrd_end > initrd_start) {
        hv_fdt_prop_u64(&f, "linux,initrd-start", initrd_start);
        hv_fdt_prop_u64(&f, "linux,initrd-end", initrd_end);
    }
    hv_fdt_end(&f);

    char name[64];
    snprintf(name, sizeof name, "memory@%llx", (unsigned long long)VMAPPLE_MEM);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_str(&f, "device_type", "memory");
    reg2(c, VMAPPLE_MEM, b->fb_size ? b->fb_base - VMAPPLE_MEM : b->ram_size);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    hv_fdt_end(&f);

    hv_fdt_begin(&f, "cpus");
    hv_fdt_prop_u32(&f, "#address-cells", 1);
    hv_fdt_prop_u32(&f, "#size-cells", 0);
    for (int i = 0; i < b->ncpus; i++) {
        snprintf(name, sizeof name, "cpu@%x", i);
        hv_fdt_begin(&f, name);
        hv_fdt_prop_str(&f, "device_type", "cpu");
        hv_fdt_prop_str(&f, "compatible", "arm,armv8");
        hv_fdt_prop_u32(&f, "reg", (uint32_t)i); /* MPIDR Aff0, as hv_vmapple_reset_cpu sets it */
        hv_fdt_prop_str(&f, "enable-method", "psci");
        hv_fdt_end(&f);
    }
    hv_fdt_end(&f);

    hv_fdt_begin(&f, "psci");
    static const char psci[] = "arm,psci-1.0\0arm,psci-0.2";
    hv_fdt_prop_strs(&f, "compatible", psci, sizeof psci);
    hv_fdt_prop_str(&f, "method", "hvc");
    hv_fdt_end(&f);

    /* PPIs: secure physical (unused), non-secure physical, virtual, hypervisor (unused); level, all CPUs. */
    hv_fdt_begin(&f, "timer");
    hv_fdt_prop_str(&f, "compatible", "arm,armv8-timer");
    uint32_t t[] = {1, 13, 0xff04, 1, HV_PPI_PTIMER - 16, 0xff04, 1, HV_PPI_VTIMER - 16, 0xff04, 1, 10, 0xff04};
    hv_fdt_prop_cells(&f, "interrupts", t, 12);
    hv_fdt_prop_empty(&f, "always-on");
    hv_fdt_end(&f);

    snprintf(name, sizeof name, "intc@%llx", (unsigned long long)VMAPPLE_GIC_DIST);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_str(&f, "compatible", "arm,gic-v3");
    hv_fdt_prop_u32(&f, "#interrupt-cells", 3);
    hv_fdt_prop_u32(&f, "#address-cells", 0);
    hv_fdt_prop_empty(&f, "interrupt-controller");
    reg2(c, VMAPPLE_GIC_DIST, 0x10000);
    reg2(c + 4, VMAPPLE_GIC_REDIST, 0x20000ull * (uint64_t)b->ncpus);
    hv_fdt_prop_cells(&f, "reg", c, 8);
    hv_fdt_prop_u32(&f, "phandle", PH_GIC);
    hv_fdt_end(&f);

    hv_fdt_begin(&f, "apb-pclk");
    hv_fdt_prop_str(&f, "compatible", "fixed-clock");
    hv_fdt_prop_u32(&f, "#clock-cells", 0);
    hv_fdt_prop_u32(&f, "clock-frequency", 24000000);
    hv_fdt_prop_str(&f, "clock-output-names", "clk24mhz");
    hv_fdt_prop_u32(&f, "phandle", PH_CLK);
    hv_fdt_end(&f);

    static const char primecell_uart[] = "arm,pl011\0arm,primecell";
    static const char clocks2[] = "uartclk\0apb_pclk";
    snprintf(name, sizeof name, "pl011@%llx", (unsigned long long)VMAPPLE_UART);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_strs(&f, "compatible", primecell_uart, sizeof primecell_uart);
    reg2(c, VMAPPLE_UART, 0x1000);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    uint32_t irq[] = {0, VMAPPLE_IRQ_UART, 4};
    hv_fdt_prop_cells(&f, "interrupts", irq, 3);
    uint32_t clk[] = {PH_CLK, PH_CLK};
    hv_fdt_prop_cells(&f, "clocks", clk, 2);
    hv_fdt_prop_strs(&f, "clock-names", clocks2, sizeof clocks2);
    hv_fdt_end(&f);

    static const char primecell_rtc[] = "arm,pl031\0arm,primecell";
    snprintf(name, sizeof name, "pl031@%llx", (unsigned long long)VMAPPLE_RTC);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_strs(&f, "compatible", primecell_rtc, sizeof primecell_rtc);
    reg2(c, VMAPPLE_RTC, 0x1000);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    irq[1] = VMAPPLE_IRQ_RTC;
    hv_fdt_prop_cells(&f, "interrupts", irq, 3);
    hv_fdt_prop_u32(&f, "clocks", PH_CLK);
    hv_fdt_prop_str(&f, "clock-names", "apb_pclk");
    hv_fdt_end(&f);

    if (b->fb_size) {
        snprintf(name, sizeof name, "framebuffer@%llx", (unsigned long long)b->fb_base);
        hv_fdt_begin(&f, name);
        hv_fdt_prop_str(&f, "compatible", "simple-framebuffer");
        reg2(c, b->fb_base, b->fb_size);
        hv_fdt_prop_cells(&f, "reg", c, 4);
        hv_fdt_prop_u32(&f, "width", b->fb_width);
        hv_fdt_prop_u32(&f, "height", b->fb_height);
        hv_fdt_prop_u32(&f, "stride", b->fb_width * 4);
        hv_fdt_prop_str(&f, "format", "x8r8g8b8");
        hv_fdt_end(&f);
    }

    /* The ECAM host: the kernel assigns BARs itself in the 32-bit window. Apple's
       host has no interrupt-map (slot s raises SPI intx_base + s whatever its pin,
       devices/pcie.c), so the map lists every slot with the pin masked off. */
    snprintf(name, sizeof name, "pcie@%llx", (unsigned long long)VMAPPLE_PCIE_ECAM);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_str(&f, "compatible", "pci-host-ecam-generic");
    hv_fdt_prop_str(&f, "device_type", "pci");
    hv_fdt_prop_u32(&f, "#address-cells", 3);
    hv_fdt_prop_u32(&f, "#size-cells", 2);
    hv_fdt_prop_u32(&f, "#interrupt-cells", 1);
    uint32_t bus[] = {0, 0xff};
    hv_fdt_prop_cells(&f, "bus-range", bus, 2);
    reg2(c, VMAPPLE_PCIE_ECAM, 0x10000000);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    uint32_t ranges[] = {0x02000000, 0, (uint32_t)VMAPPLE_PCIE_MMIO, 0, (uint32_t)VMAPPLE_PCIE_MMIO, 0, 0x1fff0000};
    hv_fdt_prop_cells(&f, "ranges", ranges, 7);
    hv_fdt_prop_empty(&f, "dma-coherent");
    uint32_t mask[] = {0xf800, 0, 0, 0};
    hv_fdt_prop_cells(&f, "interrupt-map-mask", mask, 4);
    int n = 0;
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        uint32_t e[] = {(uint32_t)s << 11, 0, 0, 0, PH_GIC, 0, (uint32_t)(VMAPPLE_IRQ_PCIE + s), 4};
        memcpy(c + n, e, sizeof e);
        n += 8;
        if (n + 8 > (int)(sizeof c / sizeof *c)) break;
    }
    hv_fdt_prop_cells(&f, "interrupt-map", c, n);
    hv_fdt_end(&f);

    hv_fdt_end(&f);
    return hv_fdt_finish(&f, len);
}

static int put_ram(HvVmapple *b, uint64_t gpa, const void *data, size_t len) {
    if (gpa < VMAPPLE_MEM || gpa - VMAPPLE_MEM + len > b->ram_size) return -1;
    memcpy(b->ram + (gpa - VMAPPLE_MEM), data, len);
    return 0;
}

int hv_linux_load(HvVmapple *b, const HvLinuxBoot *lb, char *err, size_t errlen) {
    size_t klen = 0;
    uint8_t *file;
    HvLinuxDiskBoot db = {0};
    if (lb->kernel) {
        if (!(file = read_file(lb->kernel, &klen))) return fail(err, errlen, "Cannot read the Linux kernel.");
    } else {
        if (!lb->disk || hv_linux_disk_boot(lb->disk, &db, err, errlen) != 0) return lb->disk ? -1 : fail(err, errlen, "No kernel and no disk.");
        file = db.kernel, klen = db.kernel_len, db.kernel = NULL;
    }

    /* What a unified kernel image carries besides the kernel. */
    char *uki_cmdline = NULL;
    uint8_t *uki_initrd = NULL;
    size_t uki_initrd_len = 0, sl;
    const uint8_t *s;
    if ((s = pe_section(file, klen, ".cmdline", &sl)) && (uki_cmdline = calloc(1, sl + 1))) {
        memcpy(uki_cmdline, s, sl);
        for (size_t i = 0; i < sl; i++) if (uki_cmdline[i] == '\n' || !uki_cmdline[i]) uki_cmdline[i] = ' ';
    }
    if (!lb->initrd && (s = pe_section(file, klen, ".initrd", &sl)) && (uki_initrd = malloc(sl)))
        memcpy(uki_initrd, s, sl), uki_initrd_len = sl;
    if (!lb->kernel) { /* the bootloader's initrd and command line win over a UKI's */
        if (db.initrd) free(uki_initrd), uki_initrd = db.initrd, uki_initrd_len = db.initrd_len, db.initrd = NULL;
        if (db.cmdline) free(uki_cmdline), uki_cmdline = db.cmdline, db.cmdline = NULL;
    }
    hv_linux_disk_boot_free(&db);

    uint8_t *img = to_image(file, &klen, 0);
    if (!img || klen < 64) {
        free(uki_cmdline), free(uki_initrd);
        return fail(err, errlen, "That is not an arm64 Linux kernel (Image, gzip, EFI zboot or unified kernel image).");
    }
    uint64_t text_offset = le64(img + 8), image_size = le64(img + 16);
    if (!image_size) image_size = klen;
    uint64_t kernel_pa = VMAPPLE_MEM + text_offset; /* RAM starts 2 MB aligned */
    uint64_t kernel_end = kernel_pa + (image_size > klen ? image_size : klen);

    uint8_t *initrd = uki_initrd;
    size_t initrd_len = uki_initrd_len;
    if (lb->initrd && !(initrd = read_file(lb->initrd, &initrd_len))) {
        free(img), free(uki_cmdline);
        return fail(err, errlen, "Cannot read the initrd.");
    }

    char *cmdline;
    if (lb->cmdline) cmdline = strdup(lb->cmdline);
    else if (uki_cmdline && !lb->kernel) cmdline = strdup(uki_cmdline); /* the bootloader's, consoles already fixed */
    else if (uki_cmdline) {
        size_t n = strlen(uki_cmdline) + sizeof LINUX_CONSOLE + 2;
        if ((cmdline = malloc(n))) snprintf(cmdline, n, "%s %s", uki_cmdline, LINUX_CONSOLE);
    } else cmdline = strdup(LINUX_CONSOLE);
    free(uki_cmdline);
    if (cmdline && lb->extra && *lb->extra) {
        size_t n = strlen(cmdline) + strlen(lb->extra) + 2;
        char *c2 = malloc(n);
        if (c2) snprintf(c2, n, "%s %s", cmdline, lb->extra);
        free(cmdline);
        cmdline = c2;
    }

    /* The tree in the last 2 MB of RAM (below the framebuffer, if any), the initrd under it. */
    uint64_t top = VMAPPLE_MEM + b->ram_size;
    if (b->fb_size) top = b->fb_base;
    uint64_t dtb_pa = (top - 2 * MB) & ~(2 * MB - 1);
    uint64_t initrd_pa = initrd_len ? ((dtb_pa - initrd_len) & ~(2 * MB - 1)) : dtb_pa;
    int r = -1;
    const char *why = "Not enough guest memory for the kernel, initrd and device tree.";
    if (initrd_pa > kernel_end + 16 * MB && put_ram(b, kernel_pa, img, klen) == 0 &&
        (!initrd_len || put_ram(b, initrd_pa, initrd, initrd_len) == 0)) {
        size_t dlen = 0;
        uint8_t *dtb = cmdline ? hv_linux_make_dtb(b, cmdline, initrd_pa, initrd_pa + initrd_len, &dlen) : NULL;
        if (dtb && dlen < 2 * MB && put_ram(b, dtb_pa, dtb, dlen) == 0) {
            if (lb->dtb_out) {
                FILE *o = fopen(lb->dtb_out, "wb");
                if (o) fwrite(dtb, 1, dlen, o), fclose(o);
            }
            HvCpu *c0 = &b->cpu[0];
            c0->pc = kernel_pa;
            c0->x[0] = dtb_pa;
            c0->x[1] = c0->x[2] = c0->x[3] = 0;
            if (b->m->log)
                fprintf(stderr, "hengevm: linux at %llx (%zu bytes), initrd at %llx (%zu), dtb at %llx (%zu): %s\n",
                        (unsigned long long)kernel_pa, klen, (unsigned long long)initrd_pa, initrd_len,
                        (unsigned long long)dtb_pa, dlen, cmdline);
            r = 0;
        } else why = "Cannot build the device tree.";
        free(dtb);
    }
    free(img), free(initrd), free(cmdline);
    return r == 0 ? 0 : fail(err, errlen, why);
}
