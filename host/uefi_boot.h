// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_UEFI_BOOT_H
#define HENGEVM_UEFI_BOOT_H

#include <stddef.h>

#include "virt.h"

/* Starting a guest from UEFI firmware on the virt board (devices/virt.h): the firmware
   image goes into the flash, and the device tree it reads its devices from goes to the
   start of RAM, where EDK2's ArmVirtQemu looks for it. PSCI comes from HengeVM over HVC. */
typedef struct HvUefiBoot {
    const char *code;     /* the firmware: QEMU's edk2-aarch64-code.fd */
    const char *dtb_out;  /* debugging: also write the generated tree here */
} HvUefiBoot;

/* Loads everything and points CPU 0 at the firmware (it resets the CPUs).
   Returns 0, or -1 with a reason in err. */
int hv_uefi_load(HvVmapple *b, const HvUefiBoot *ub, char *err, size_t errlen);

/* The device tree on its own (tests): malloc'd blob, *len set. */
unsigned char *hv_uefi_make_dtb(const HvVmapple *b, size_t *len);

#endif
