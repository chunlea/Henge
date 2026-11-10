// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_ACPI_H
#define HENGEVM_ACPI_H

#include "virt.h"

/* The ACPI tables of the UEFI board (devices/virt.h), which is how Windows learns what the
   machine has: it does not read device trees. They are handed to the firmware the way QEMU
   hands over its own: three fw_cfg files, the tables in one blob, the root pointer, and a list
   of commands (etc/table-loader) telling the firmware where to put them and which addresses
   and checksums to fill in afterwards (QEMU's hw/acpi/bios-linker-loader.c). EDK2 then
   installs each table, and stops offering the device tree to the system it starts.

   Tables: DSDT (processors, the serial port, the PCIe host and the interrupt each slot
   raises), FADT (hardware-reduced, PSCI over HVC), MADT (GICv3: one CPU interface and
   redistributor per processor), GTDT (the timers), MCFG (ECAM), SPCR and DBG2 (the serial
   port as console and debug port). Returns 0, or -1. */
int hv_acpi_install(HvVmapple *b);

#endif
