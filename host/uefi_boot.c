// SPDX-License-Identifier: GPL-2.0-or-later
#include "uefi_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fdt.h"
#include "timer.h"

static int fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return -1;
}

static void reg2(uint32_t *c, uint64_t addr, uint64_t size) {
    c[0] = (uint32_t)(addr >> 32), c[1] = (uint32_t)addr, c[2] = (uint32_t)(size >> 32), c[3] = (uint32_t)size;
}

enum { PH_GIC = 1, PH_CLK = 2 };

/* The nodes are the ones QEMU's virt machine has, under the names it gives them:
   the firmware finds each by its compatible string. */
unsigned char *hv_uefi_make_dtb(const HvVmapple *b, size_t *len) {
    HvFdt f;
    uint32_t c[16 + HV_PCI_SLOTS * 8];
    char name[64];
    hv_fdt_init(&f);
    hv_fdt_begin(&f, "");
    hv_fdt_prop_u32(&f, "#address-cells", 2);
    hv_fdt_prop_u32(&f, "#size-cells", 2);
    hv_fdt_prop_str(&f, "compatible", "linux,dummy-virt");
    hv_fdt_prop_str(&f, "model", "Henge");
    hv_fdt_prop_u32(&f, "interrupt-parent", PH_GIC);

    hv_fdt_begin(&f, "chosen");
    hv_fdt_prop_str(&f, "stdout-path", "/pl011@9000000");
    hv_fdt_end(&f);

    snprintf(name, sizeof name, "memory@%llx", (unsigned long long)VIRT_MEM);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_str(&f, "device_type", "memory");
    reg2(c, VIRT_MEM, b->ram_size);
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
        hv_fdt_prop_u32(&f, "reg", (uint32_t)i); /* MPIDR Aff0, as hv_virt_reset_cpu sets it */
        hv_fdt_prop_str(&f, "enable-method", "psci");
        hv_fdt_end(&f);
    }
    hv_fdt_end(&f);

    hv_fdt_begin(&f, "psci");
    static const char psci[] = "arm,psci-1.0\0arm,psci-0.2\0arm,psci";
    hv_fdt_prop_strs(&f, "compatible", psci, sizeof psci);
    hv_fdt_prop_str(&f, "method", "hvc");
    hv_fdt_prop_u32(&f, "cpu_on", 0xc4000003);
    hv_fdt_prop_u32(&f, "cpu_off", 0x84000002);
    hv_fdt_end(&f);

    /* PPIs: secure physical (unused), non-secure physical, virtual, hypervisor (unused); level, all CPUs. */
    hv_fdt_begin(&f, "timer");
    static const char timer[] = "arm,armv8-timer\0arm,armv7-timer";
    hv_fdt_prop_strs(&f, "compatible", timer, sizeof timer);
    uint32_t t[] = {1, 13, 0xff04, 1, HV_PPI_PTIMER - 16, 0xff04, 1, HV_PPI_VTIMER - 16, 0xff04, 1, 10, 0xff04};
    hv_fdt_prop_cells(&f, "interrupts", t, 12);
    hv_fdt_prop_empty(&f, "always-on");
    hv_fdt_end(&f);

    snprintf(name, sizeof name, "intc@%llx", (unsigned long long)VIRT_GIC_DIST);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_str(&f, "compatible", "arm,gic-v3");
    hv_fdt_prop_u32(&f, "#interrupt-cells", 3);
    hv_fdt_prop_u32(&f, "#address-cells", 2);
    hv_fdt_prop_u32(&f, "#size-cells", 2);
    hv_fdt_prop_empty(&f, "ranges");
    hv_fdt_prop_empty(&f, "interrupt-controller");
    hv_fdt_prop_u32(&f, "#redistributor-regions", 1);
    reg2(c, VIRT_GIC_DIST, 0x10000);
    reg2(c + 4, VIRT_GIC_REDIST, 0x20000ull * (uint64_t)b->ncpus);
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
    snprintf(name, sizeof name, "pl011@%llx", (unsigned long long)VIRT_UART);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_strs(&f, "compatible", primecell_uart, sizeof primecell_uart);
    reg2(c, VIRT_UART, 0x1000);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    uint32_t irq[] = {0, VIRT_IRQ_UART, 4};
    hv_fdt_prop_cells(&f, "interrupts", irq, 3);
    uint32_t clk[] = {PH_CLK, PH_CLK};
    hv_fdt_prop_cells(&f, "clocks", clk, 2);
    hv_fdt_prop_strs(&f, "clock-names", clocks2, sizeof clocks2);
    hv_fdt_end(&f);

    static const char primecell_rtc[] = "arm,pl031\0arm,primecell";
    snprintf(name, sizeof name, "pl031@%llx", (unsigned long long)VIRT_RTC);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_strs(&f, "compatible", primecell_rtc, sizeof primecell_rtc);
    reg2(c, VIRT_RTC, 0x1000);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    irq[1] = VIRT_IRQ_RTC;
    hv_fdt_prop_cells(&f, "interrupts", irq, 3);
    hv_fdt_prop_u32(&f, "clocks", PH_CLK);
    hv_fdt_prop_str(&f, "clock-names", "apb_pclk");
    hv_fdt_end(&f);

    snprintf(name, sizeof name, "fw-cfg@%llx", (unsigned long long)VIRT_FW_CFG);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_str(&f, "compatible", "qemu,fw-cfg-mmio");
    reg2(c, VIRT_FW_CFG, 0x18);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    hv_fdt_prop_empty(&f, "dma-coherent");
    hv_fdt_end(&f);

    /* Both flash chips in one node, as QEMU has them; the firmware's variable driver takes the second. */
    hv_fdt_begin(&f, "flash@0");
    hv_fdt_prop_str(&f, "compatible", "cfi-flash");
    reg2(c, VIRT_FLASH_CODE, VIRT_FLASH_SIZE);
    reg2(c + 4, VIRT_FLASH_VARS, VIRT_FLASH_SIZE);
    hv_fdt_prop_cells(&f, "reg", c, 8);
    hv_fdt_prop_u32(&f, "bank-width", 4);
    hv_fdt_end(&f);

    /* The ECAM host. Slot s raises SPI VIRT_IRQ_PCIE + s whatever its pin (devices/pcie.c), so the
       map lists every slot with the pin masked off. */
    snprintf(name, sizeof name, "pcie@%llx", (unsigned long long)VIRT_PCIE_MMIO);
    hv_fdt_begin(&f, name);
    hv_fdt_prop_str(&f, "compatible", "pci-host-ecam-generic");
    hv_fdt_prop_str(&f, "device_type", "pci");
    hv_fdt_prop_u32(&f, "#address-cells", 3);
    hv_fdt_prop_u32(&f, "#size-cells", 2);
    hv_fdt_prop_u32(&f, "#interrupt-cells", 1);
    hv_fdt_prop_u32(&f, "linux,pci-domain", 0);
    uint32_t bus[] = {0, (uint32_t)(VIRT_PCIE_ECAM_SIZE >> 20) - 1};
    hv_fdt_prop_cells(&f, "bus-range", bus, 2);
    reg2(c, VIRT_PCIE_ECAM, VIRT_PCIE_ECAM_SIZE);
    hv_fdt_prop_cells(&f, "reg", c, 4);
    uint32_t ranges[] = {0x01000000, 0, 0, 0, (uint32_t)VIRT_PCIE_PIO, 0, 0x10000,
                         0x02000000, 0, (uint32_t)VIRT_PCIE_MMIO, 0, (uint32_t)VIRT_PCIE_MMIO, 0, (uint32_t)VIRT_PCIE_MMIO_SIZE};
    hv_fdt_prop_cells(&f, "ranges", ranges, 14);
    hv_fdt_prop_empty(&f, "dma-coherent");
    uint32_t mask[] = {0xf800, 0, 0, 0};
    hv_fdt_prop_cells(&f, "interrupt-map-mask", mask, 4);
    /* A parent unit address of two cells (the GIC's #address-cells), then its three interrupt cells. */
    int n = 0;
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        uint32_t e[] = {(uint32_t)s << 11, 0, 0, 0, PH_GIC, 0, 0, 0, (uint32_t)(VIRT_IRQ_PCIE + s), 4};
        if (n + 10 > (int)(sizeof c / sizeof *c)) break;
        memcpy(c + n, e, sizeof e);
        n += 10;
    }
    hv_fdt_prop_cells(&f, "interrupt-map", c, n);
    hv_fdt_end(&f);

    hv_fdt_end(&f);
    return hv_fdt_finish(&f, len);
}

int hv_uefi_load(HvVmapple *b, const HvUefiBoot *ub, char *err, size_t errlen) {
    if (!ub->code || hv_virt_load_firmware(b, ub->code) != 0)
        return fail(err, errlen, "Cannot read the UEFI firmware (edk2-aarch64-code.fd).");
    size_t dlen = 0;
    uint8_t *dtb = hv_uefi_make_dtb(b, &dlen);
    if (!dtb || dlen > (1u << 20) || dlen > b->ram_size) {
        free(dtb);
        return fail(err, errlen, "Cannot build the device tree.");
    }
    memcpy(b->ram, dtb, dlen); /* the start of RAM */
    if (ub->dtb_out) {
        FILE *o = fopen(ub->dtb_out, "wb");
        if (o) fwrite(dtb, 1, dlen, o), fclose(o);
    }
    free(dtb);
    hv_virt_reset_cpu(b);
    b->cpu[0].x[0] = VIRT_MEM; /* what QEMU passes; the firmware knows the address anyway */
    return 0;
}
