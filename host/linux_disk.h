// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_LINUX_DISK_H
#define HENGEVM_LINUX_DISK_H

#include <stddef.h>
#include <stdint.h>

/* What a Linux disk would boot through its bootloader, read on the host: the
   GPT's EFI system partition (FAT12/16/32, long names), and on it Limine's
   limine.conf (Omarchy) or a unified kernel image under EFI/Linux. HengeVM
   has no UEFI, so this stands in for the firmware and bootloader.

   On success the kernel, initrd (or NULL) and command line are malloc'd. */
typedef struct HvLinuxDiskBoot {
    uint8_t *kernel, *initrd;
    size_t kernel_len, initrd_len;
    char *cmdline; /* may be NULL (a UKI carries its own) */
} HvLinuxDiskBoot;

int hv_linux_disk_boot(const char *disk_path, HvLinuxDiskBoot *out, char *err, size_t errlen);
void hv_linux_disk_boot_free(HvLinuxDiskBoot *b);

/* One file from the disk's EFI system partition (tests, tools), malloc'd. */
uint8_t *hv_linux_disk_read_esp(const char *disk_path, const char *path, size_t *len);

#endif
