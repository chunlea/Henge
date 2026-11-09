// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_LINUX_BOOT_H
#define HENGEVM_LINUX_BOOT_H

#include <stddef.h>

#include "vmapple.h"

/* Booting Linux on the same board instead of AVPBooter: the kernel is loaded
   straight into guest RAM with a device tree describing the devices Linux has
   drivers for (GICv3, PL011, PL031, the generic ECAM PCIe host with its virtio
   devices), as the arm64 boot protocol asks (Documentation/arch/arm64/booting.rst).
   There is no firmware: PSCI comes from HengeVM over HVC, as for macOS.

   kernel: an arm64 Image, gzip'd or not, an EFI zboot image (what distributions
   ship as vmlinuz), or a unified kernel image (a PE with .linux, and optionally
   .initrd and .cmdline, which is how Omarchy boots). */
typedef struct HvLinuxBoot {
    const char *kernel;
    const char *initrd;   /* NULL: none, or the UKI's own */
    const char *cmdline;  /* NULL: the UKI's own plus the serial console, or just the serial console */
    const char *dtb_out;  /* debugging: also write the generated tree here */
} HvLinuxBoot;

/* Loads everything and points CPU 0 at the kernel (call after hv_vmapple_reset_cpu).
   Returns 0, or -1 with a reason in err. */
int hv_linux_load(HvVmapple *b, const HvLinuxBoot *lb, char *err, size_t errlen);

/* The device tree on its own (tests): malloc'd blob, *len set. */
unsigned char *hv_linux_make_dtb(const HvVmapple *b, const char *cmdline, unsigned long long initrd_start,
                                 unsigned long long initrd_end, size_t *len);

#endif
