#ifndef HENGEVM_NVME_H
#define HENGEVM_NVME_H

#include <stdint.h>

#include "disk.h"
#include "pcie.h"

/* An NVMe controller (NVM Express 1.4) with one namespace, the disk: the storage Windows
   has a driver for out of the box (stornvme), as it has none for virtio. EDK2's firmware
   has one too (NvmExpressDxe), so the same disk is what the machine starts from.

   One interrupt, the PCI pin (no MSI: the board has no ITS), which every completion queue
   that asks for interrupts shares. Commands are carried out where the doorbell write lands,
   on the CPU thread with the device lock held; reads and writes go straight between the
   disk file and guest memory. */
#define HV_NVME_QUEUES 16 /* I/O queue pairs a guest may make, besides the admin pair (queue 0) */

typedef struct HvNvmeQueue {
    uint64_t base;     /* guest PA of the ring; 0: the queue does not exist */
    uint32_t size;     /* entries */
    uint32_t head, tail;
    uint16_t cqid;     /* submission queues: where completions go */
    int ien;           /* completion queues: interrupts wanted */
    int phase;         /* completion queues: the phase bit of the next entry */
} HvNvmeQueue;

typedef struct HvNvme {
    HvPciDev pci;
    HvMachine *m;
    HvDisk *disk;
    uint64_t cap;
    uint32_t cc, csts, aqa, intms;
    uint64_t asq, acq;
    uint32_t page;     /* memory page size the driver chose (CC.MPS), bytes */
    HvNvmeQueue sq[HV_NVME_QUEUES + 1], cq[HV_NVME_QUEUES + 1];
    uint8_t *bounce;   /* a transfer's worth of data on its way between the disk and guest pages */
    uint64_t reads, writes, errors;
} HvNvme;

HvNvme *hv_nvme_create(HvMachine *m, HvPcie *bus, HvDisk *disk);
void hv_nvme_destroy(HvNvme *n);

#endif
