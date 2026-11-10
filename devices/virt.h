#ifndef HENGEVM_VIRT_H
#define HENGEVM_VIRT_H

#include <stdint.h>

#include "fw_cfg.h"
#include "nvme.h"
#include "pflash.h"
#include "vmapple.h"

/* The board for guests that start from UEFI firmware (Windows on Arm): the memory map of
   QEMU's `virt` machine (hw/arm/virt.c, with highmem off), because that is the machine the
   EDK2 firmware QEMU ships (ArmVirtQemu) is built for. It runs from flash at 0, expects its
   device tree at the start of RAM and maps 0x08000000-0x0fffffff for the devices it reaches
   before it has read the tree. It is the same engine and the same device models as the
   virtual Mac, in an HvVmapple with the Apple-only devices left out. */
#define VIRT_FLASH_CODE  0x00000000ull /* the firmware (read-only) */
#define VIRT_FLASH_VARS  0x04000000ull /* its variables */
#define VIRT_FLASH_SIZE  0x04000000ull /* 64 MB each */
#define VIRT_GIC_DIST    0x08000000ull
#define VIRT_GIC_REDIST  0x080a0000ull
#define VIRT_UART        0x09000000ull
#define VIRT_RTC         0x09010000ull
#define VIRT_FW_CFG      0x09020000ull
#define VIRT_PCIE_MMIO   0x10000000ull
#define VIRT_PCIE_MMIO_SIZE 0x2eff0000ull
#define VIRT_PCIE_PIO    0x3eff0000ull /* port I/O space: no device here has any */
#define VIRT_PCIE_ECAM   0x3f000000ull
#define VIRT_PCIE_ECAM_SIZE 0x01000000ull /* buses 0-15 */
#define VIRT_MEM         0x40000000ull

/* SPI numbers; the INTID is 32 more. */
#define VIRT_IRQ_UART 1
#define VIRT_IRQ_RTC 2
#define VIRT_IRQ_PCIE 0x20 /* slot s raises this + s (devices/pcie.c) */

/* The display is QEMU's ramfb: the firmware's driver (QemuRamfbDxe) picks memory for a
   picture and writes where it is, and its shape, to the fw_cfg file etc/ramfb. Windows
   draws into the same memory until it has a display driver of its own, through the
   framebuffer UEFI's Graphics Output Protocol hands it. */
typedef struct HvVirtParams {
    int ncpus;
    uint64_t ram_size;
    HvDisk *disk;            /* NULL: none. An NVMe disk: what Windows has a driver for */
    HvDisk *cdrom;           /* a second, read-only disk: installation media (NULL: none) */
    const uint8_t *mac;      /* NULL: no network card */
    const char *vars_path;   /* where the firmware's variables are kept (NULL: nowhere) */
    uint32_t screen_width, screen_height; /* the display's size; 0: the firmware's own choice (800 x 600) */
} HvVirtParams;

int hv_virt_create(HvVmapple *b, HvMachine *m, HvCpu *cpus, const HvVirtParams *p);
/* The firmware image (QEMU's edk2-aarch64-code.fd, or any ArmVirtQemu build), up to 64 MB. */
int hv_virt_load_firmware(HvVmapple *b, const char *path);
/* CPU reset: EL1, MMU off, at the start of the flash; the others wait for PSCI CPU_ON. */
void hv_virt_reset_cpu(HvVmapple *b);

#endif
