#ifndef HENGEVM_PFLASH_H
#define HENGEVM_PFLASH_H

#include <stdint.h>

#include "machine.h"

/* A NOR flash chip with Intel's command set (CFI 01, as QEMU's hw/block/pflash_cfi01.c),
   which is where UEFI firmware keeps its variables: the boot entries an installer adds
   have to be there after a restart. Only the start of the chip is kept (EDK2's variable
   store is its first 768 KB); the rest reads as erased and ignores writes. The contents
   are written back to the file whenever the firmware changes them. */
#define HV_PFLASH_KEPT (1u << 20)
#define HV_PFLASH_BLOCK (256u << 10)

typedef struct HvPflash {
    uint8_t *data;       /* HV_PFLASH_KEPT bytes */
    uint64_t size;
    char *path;          /* NULL: not kept past this run */
    int mode;            /* the command being carried out (0: reads give the contents) */
    uint8_t status;
    uint32_t left;       /* buffered program: words still to come */
    uint64_t block;      /* the block a buffered program or an erase was started on */
    int dirty;
} HvPflash;

/* path may be NULL, or name a file that does not exist yet (the chip starts erased). */
HvPflash *hv_pflash_create(HvMachine *m, uint64_t pa, uint64_t size, const char *path);
void hv_pflash_destroy(HvPflash *f);

#endif
