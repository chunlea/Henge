#ifndef HENGEVM_NVME_H
#define HENGEVM_NVME_H

#include <pthread.h>
#include <stdint.h>

#include "disk.h"
#include "pcie.h"

/* An NVMe controller (NVM Express 1.4) with one namespace, the disk: the storage Windows
   has a driver for out of the box (stornvme), as it has none for virtio. EDK2's firmware
   has one too (NvmExpressDxe), so the same disk is what the machine starts from.

   One interrupt, the PCI pin (no MSI: the board has no ITS), which every completion queue
   that asks for interrupts shares. Admin commands are carried out where the doorbell write
   lands. Reads and writes are a worker thread's, as a real controller's DMA is nobody's on
   the CPU: the CPU thread takes them off the queue and, from the board's tick
   (hv_nvme_poll), posts their completions. A guest CPU never waits for the host's disk,
   which Windows would not forgive it (a DPC that takes too long is a
   DPC_WATCHDOG_VIOLATION). */
#define HV_NVME_QUEUES 16 /* I/O queue pairs a guest may make, besides the admin pair (queue 0) */
#define HV_NVME_RING 1024 /* I/O commands taken and not yet completed */

typedef struct HvNvmeReq {
    uint8_t cmd[64];
    uint16_t sqid;
    int status;        /* set by the worker */
    int done;          /* the worker has finished it (release store); the CPU thread posts it */
    int posted;
} HvNvmeReq;

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
    uint8_t *bounce;   /* a transfer's worth of data on its way between the disk and guest pages (the worker's) */
    uint64_t reads, writes, errors;
    HvNvmeReq *ring;
    uint32_t submitted;  /* CPU thread: commands handed to the worker */
    uint32_t claimed;    /* worker: commands taken (under mu) */
    uint32_t posted;     /* CPU thread: every command before this has its completion posted */
    uint32_t waiting;    /* submission queues with commands left on them because the ring was full, by bit */
    int busy;            /* worker: one is being carried out */
    pthread_mutex_t mu;
    pthread_cond_t cv, idle_cv;
    pthread_t worker;
    int stopping, worker_started;
    struct { uint32_t off, val; } trace[32]; /* the last register writes (hv_nvme_dump) */
    uint32_t ntrace;
} HvNvme;

/* Debugging: the controller's registers, queues and last register writes, to stderr. */
void hv_nvme_dump(HvNvme *n);

HvNvme *hv_nvme_create(HvMachine *m, HvPcie *bus, HvDisk *disk);
/* CPU thread: post the completions of finished commands and raise the interrupt. */
void hv_nvme_poll(HvNvme *n);
/* After CPUs stop: finish what was submitted and join the worker, before the disk is flushed or freed. */
void hv_nvme_stop(HvNvme *n);
void hv_nvme_destroy(HvNvme *n);

#endif
