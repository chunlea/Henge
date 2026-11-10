#ifndef HENGEVM_FW_CFG_H
#define HENGEVM_FW_CFG_H

#include <stdint.h>

#include "machine.h"

/* QEMU's firmware configuration device, the memory-mapped kind (docs/specs/fw_cfg.rst in QEMU):
   how UEFI firmware built for QEMU's virt machine (EDK2's ArmVirtQemu) is handed its ACPI and
   SMBIOS tables, and where its display driver says which memory holds the picture (etc/ramfb).
   Registers: +0 data (bytes of the selected item, in order), +8 selector (big-endian, 16 bits),
   +16 the address of a DMA request (big-endian, 64 bits; the low half's write starts it). */
#define HV_FWCFG_FILES 24

typedef struct HvFwCfgFile {
    char name[56];
    uint8_t *data;       /* owned */
    uint32_t size;
    /* The guest wrote the item (DMA only, as in QEMU): data holds what it wrote. */
    void (*written)(void *opaque);
    void *opaque;
} HvFwCfgFile;

typedef struct HvFwCfg {
    HvMachine *m;
    uint16_t sel;
    uint32_t off;
    uint32_t dma_high;
    uint16_t ncpus;
    HvFwCfgFile file[HV_FWCFG_FILES];
    int nfiles;
    uint8_t *dir;        /* the directory item, rebuilt when a file is added */
    uint32_t dir_size;
} HvFwCfg;

HvFwCfg *hv_fw_cfg_create(HvMachine *m, uint64_t pa, int ncpus);
/* Adds a file (the data is copied). written may be NULL: the guest then cannot write it. Returns 0, or -1 when full. */
int hv_fw_cfg_add_file(HvFwCfg *f, const char *name, const void *data, uint32_t size, void (*written)(void *opaque),
                       void *opaque);
HvFwCfgFile *hv_fw_cfg_find(HvFwCfg *f, const char *name);
void hv_fw_cfg_destroy(HvFwCfg *f);

#endif
