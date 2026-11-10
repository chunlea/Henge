#include "nvme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR_SIZE 0x4000u
#define DOORBELLS 0x1000u
#define BLOCK 512u
#define MDTS 8                         /* a command carries at most 2^8 pages of 4 KB: 1 MB */
#define BOUNCE ((size_t)4096 << MDTS)

/* Status: the code, its type in bits 10..8, and "do not retry" in bit 14. */
enum {
    SC_OK = 0,
    SC_BAD_OPCODE = 0x4001,
    SC_BAD_FIELD = 0x4002,
    SC_DATA_ERROR = 0x4004,
    SC_BAD_NAMESPACE = 0x400b,
    SC_LBA_RANGE = 0x4080,
    SC_WRITE_PROTECTED = 0x4020,
    SC_BAD_QUEUE = 0x4101,
    SC_BAD_QUEUE_SIZE = 0x4102,
    SC_NO_COMPLETION = -1,             /* an asynchronous event request: answered only if something happens */
};

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void update_irq(HvNvme *n) {
    int level = 0;
    for (int q = 0; q <= HV_NVME_QUEUES; q++)
        if (n->cq[q].base && n->cq[q].ien && n->cq[q].head != n->cq[q].tail) level = 1;
    hv_pci_set_intx(&n->pci, level && !(n->intms & 1));
}

static int copy(HvNvme *n, uint64_t pa, uint8_t *buf, uint64_t len, int to_guest) {
    uint8_t *p = hv_phys_ptr(n->m, pa, len);
    if (!p) return -1;
    if (to_guest) {
        memcpy(p, buf, len);
        hv_dma_written(n->m, pa, len);
    } else memcpy(buf, p, len);
    return 0;
}

/* A command's data, between buf and the guest pages its two PRP fields name: the first may start
   inside a page; the second is the second page, or when there are more, a list of the rest (whose
   last entry, if the list fills its page, points to the list's next page). */
static int prp_xfer(HvNvme *n, uint64_t prp1, uint64_t prp2, uint8_t *buf, uint64_t len, int to_guest) {
    uint64_t ps = n->page, first = ps - (prp1 & (ps - 1));
    if (first > len) first = len;
    if (copy(n, prp1, buf, first, to_guest)) return -1;
    buf += first, len -= first;
    if (!len) return 0;
    if (len <= ps) return copy(n, prp2, buf, len, to_guest);
    uint64_t list = prp2 & ~7ull;
    for (;;) {
        uint64_t e;
        if (hv_phys_read(n->m, list, &e, 8)) return -1;
        if (((list + 8) & (ps - 1)) == 0 && len > ps) { /* the page's last entry, and more than a page to go */
            list = e & ~7ull;
            continue;
        }
        uint64_t part = len < ps ? len : ps;
        if (copy(n, e, buf, part, to_guest)) return -1;
        buf += part, len -= part;
        if (!len) return 0;
        list += 8;
    }
}

static void post(HvNvme *n, int sqid, uint16_t cid, int status, uint32_t result) {
    HvNvmeQueue *sq = &n->sq[sqid], *cq = &n->cq[sq->cqid];
    if (!cq->base) return;
    uint8_t e[16] = {0};
    put32(e, result);
    put16(e + 8, (uint16_t)sq->head);
    put16(e + 10, (uint16_t)sqid);
    put16(e + 12, cid);
    put16(e + 14, (uint16_t)(status << 1 | cq->phase));
    hv_phys_write(n->m, cq->base + (uint64_t)cq->tail * 16, e, 16);
    if (++cq->tail == cq->size) cq->tail = 0, cq->phase ^= 1;
}

static void ascii(uint8_t *to, size_t len, const char *s) {
    memset(to, ' ', len);
    memcpy(to, s, strlen(s) < len ? strlen(s) : len);
}

static int identify(HvNvme *n, const uint8_t *c) {
    uint8_t *d = n->bounce;
    uint32_t nsid = get32(c + 4);
    uint64_t blocks = n->disk->size / BLOCK;
    memset(d, 0, 4096);
    switch (c[40]) {
    case 0x00: /* the namespace */
        if (nsid != 1) break; /* one that does not exist: zeros */
        put64(d, blocks), put64(d + 8, blocks), put64(d + 16, blocks);
        d[99] = n->disk->readonly ? 1 : 0;
        memcpy(d + 120, "\x02HENGE\x00\x01", 8);    /* EUI-64 */
        d[130] = 9;                                 /* the one LBA format: 512-byte blocks, no metadata */
        break;
    case 0x01: /* the controller */
        put16(d, 0x1b36), put16(d + 2, 0x1af4);
        ascii(d + 4, 20, "HENGE0001");
        ascii(d + 24, 40, "Henge NVMe Disk");
        ascii(d + 64, 8, "1.0");
        d[77] = MDTS;
        put32(d + 80, 0x00010400);
        d[111] = 1;                                 /* an I/O controller */
        d[258] = 3, d[259] = 3;                     /* aborts and asynchronous event requests at once, less one */
        d[260] = 0x03;                              /* one firmware slot, read-only */
        d[512] = 0x66, d[513] = 0x44;               /* 64-byte commands, 16-byte completions */
        put32(d + 516, 1);                          /* namespaces */
        put16(d + 520, 0x0c);                       /* dataset management and write zeroes */
        d[525] = 1;                                 /* a volatile write cache: flush means something */
        snprintf((char *)d + 768, 256, "nqn.2026-11.dev.henge:nvme:disk");
        put16(d + 2048, 2500);                      /* power state 0: 25 W */
        break;
    case 0x02: /* the namespaces with an id above nsid */
        if (nsid < 1) put32(d, 1);
        break;
    case 0x03: /* the namespace's identifiers: its EUI-64 */
        if (nsid != 1) return SC_BAD_NAMESPACE;
        d[0] = 1, d[1] = 8;
        memcpy(d + 4, "\x02HENGE\x00\x01", 8);
        break;
    default: return SC_BAD_FIELD;
    }
    return prp_xfer(n, get64(c + 24), get64(c + 32), d, 4096, 1) ? SC_DATA_ERROR : SC_OK;
}

static int admin(HvNvme *n, const uint8_t *c, uint32_t *result) {
    uint32_t cdw10 = get32(c + 40), cdw11 = get32(c + 44);
    uint32_t qid = cdw10 & 0xffff, qsize = (cdw10 >> 16) + 1;
    switch (c[0]) {
    case 0x00: /* delete a submission queue */
        if (!qid || qid > HV_NVME_QUEUES || !n->sq[qid].base) return SC_BAD_QUEUE;
        n->sq[qid] = (HvNvmeQueue){0};
        return SC_OK;
    case 0x01: /* create one: its completions go to queue cdw11 >> 16 */
        if (!qid || qid > HV_NVME_QUEUES || n->sq[qid].base) return SC_BAD_QUEUE;
        if ((cdw11 >> 16) > HV_NVME_QUEUES || !n->cq[cdw11 >> 16].base) return SC_BAD_QUEUE;
        if (qsize < 2) return SC_BAD_QUEUE_SIZE;
        n->sq[qid] = (HvNvmeQueue){.base = get64(c + 24) & ~(uint64_t)(n->page - 1), .size = qsize, .cqid = (uint16_t)(cdw11 >> 16)};
        return SC_OK;
    case 0x04: /* delete a completion queue */
        if (!qid || qid > HV_NVME_QUEUES || !n->cq[qid].base) return SC_BAD_QUEUE;
        n->cq[qid] = (HvNvmeQueue){0};
        return SC_OK;
    case 0x05: /* create one */
        if (!qid || qid > HV_NVME_QUEUES || n->cq[qid].base) return SC_BAD_QUEUE;
        if (qsize < 2) return SC_BAD_QUEUE_SIZE;
        n->cq[qid] = (HvNvmeQueue){.base = get64(c + 24) & ~(uint64_t)(n->page - 1), .size = qsize, .ien = (cdw11 >> 1) & 1, .phase = 1};
        return SC_OK;
    case 0x02: { /* a log page: no errors, a healthy disk, firmware in slot 1 */
        uint64_t len = ((uint64_t)((cdw10 >> 16) | (cdw11 & 0xffff) << 16) + 1) * 4;
        if (len > BOUNCE) len = BOUNCE;
        memset(n->bounce, 0, len);
        return prp_xfer(n, get64(c + 24), get64(c + 32), n->bounce, len, 1) ? SC_DATA_ERROR : SC_OK;
    }
    case 0x06: return identify(n, c);
    case 0x08: /* abort: the command it names has finished by now */
        *result = 1;
        return SC_OK;
    case 0x09: /* set a feature: the number of queues is the only one with an answer */
    case 0x0a: /* get a feature */
        if ((cdw10 & 0xff) == 0x07) *result = (HV_NVME_QUEUES - 1) << 16 | (HV_NVME_QUEUES - 1);
        else if ((cdw10 & 0xff) == 0x06 && c[0] == 0x0a) *result = 1; /* the write cache is on */
        return SC_OK;
    case 0x0c: return SC_NO_COMPLETION;
    default: return SC_BAD_OPCODE;
    }
}

static int io(HvNvme *n, const uint8_t *c) {
    if (c[0] == 0x00) return hv_disk_flush(n->disk) ? SC_DATA_ERROR : SC_OK;
    if (get32(c + 4) != 1) return SC_BAD_NAMESPACE;
    if (c[0] == 0x09) return SC_OK; /* dataset management: a hint that blocks are no longer used */
    if (c[0] != 0x01 && c[0] != 0x02 && c[0] != 0x08) return SC_BAD_OPCODE;
    uint64_t lba = get64(c + 40), len = (uint64_t)((get32(c + 48) & 0xffff) + 1) * BLOCK;
    if (lba > n->disk->size / BLOCK || len > n->disk->size - lba * BLOCK) return SC_LBA_RANGE;
    if (c[0] != 0x02 && n->disk->readonly) return SC_WRITE_PROTECTED;
    if (c[0] == 0x08) { /* write zeroes */
        memset(n->bounce, 0, len < BOUNCE ? len : BOUNCE);
        for (uint64_t done = 0; done < len; done += BOUNCE)
            if (hv_disk_write(n->disk, lba * BLOCK + done, n->bounce, len - done < BOUNCE ? len - done : BOUNCE)) return SC_DATA_ERROR;
        return SC_OK;
    }
    if (len > BOUNCE) return SC_BAD_FIELD;
    uint64_t prp1 = get64(c + 24), prp2 = get64(c + 32);
    if (c[0] == 0x02) {
        n->reads++;
        if (hv_disk_read(n->disk, lba * BLOCK, n->bounce, len)) return SC_DATA_ERROR;
        return prp_xfer(n, prp1, prp2, n->bounce, len, 1) ? SC_DATA_ERROR : SC_OK;
    }
    n->writes++;
    if (prp_xfer(n, prp1, prp2, n->bounce, len, 0)) return SC_DATA_ERROR;
    return hv_disk_write(n->disk, lba * BLOCK, n->bounce, len) ? SC_DATA_ERROR : SC_OK;
}

/* The driver moved a submission queue's tail: carry out what is new. */
static void run(HvNvme *n, int qid) {
    HvNvmeQueue *sq = &n->sq[qid];
    while (sq->base && sq->head != sq->tail) {
        uint8_t c[64];
        if (hv_phys_read(n->m, sq->base + (uint64_t)sq->head * 64, c, 64)) break;
        if (++sq->head == sq->size) sq->head = 0;
        uint32_t result = 0;
        int status = qid ? io(n, c) : admin(n, c, &result);
        if (status != SC_OK && status != SC_NO_COMPLETION) {
            n->errors++;
            if (n->m->log) fprintf(stderr, "hengevm: nvme: %s command %02x failed (status %x)\n", qid ? "i/o" : "admin", c[0], status & 0x7ff);
        }
        if (status != SC_NO_COMPLETION) post(n, qid, (uint16_t)(c[2] | c[3] << 8), status, result);
    }
    update_irq(n);
}

static void enable(HvNvme *n, int on) {
    memset(n->sq, 0, sizeof n->sq);
    memset(n->cq, 0, sizeof n->cq);
    n->csts = 0;
    if (!on) {
        n->intms = 0;
        update_irq(n);
        return;
    }
    n->page = 1u << (12 + ((n->cc >> 7) & 15));
    n->sq[0] = (HvNvmeQueue){.base = n->asq & ~0xfffull, .size = (n->aqa & 0xfff) + 1};
    n->cq[0] = (HvNvmeQueue){.base = n->acq & ~0xfffull, .size = ((n->aqa >> 16) & 0xfff) + 1, .ien = 1, .phase = 1};
    n->csts = 1; /* ready */
}

static uint64_t bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    (void)bar;
    HvNvme *n = d->opaque;
    uint64_t v;
    switch (off & ~7ull) {
    case 0x00: v = n->cap; break;
    case 0x08: v = 0x00010400 | (uint64_t)n->intms << 32; break; /* version 1.4; +0xc the interrupt mask */
    case 0x10: v = (uint64_t)n->cc << 32 | n->intms; break;      /* +0x10 the mask again (its "clear" register), +0x14 CC */
    case 0x18: v = (uint64_t)n->csts << 32; break;
    case 0x20: v = (uint64_t)n->aqa << 32; break;
    case 0x28: v = n->asq; break;
    case 0x30: v = n->acq; break;
    default: v = 0; break;
    }
    v >>= (off & 7) * 8;
    return size >= 8 ? v : v & ((1ull << (size * 8)) - 1);
}

static void bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t v, unsigned size) {
    (void)bar;
    HvNvme *n = d->opaque;
    if (off >= DOORBELLS) {
        uint32_t i = (uint32_t)(off - DOORBELLS) / 4, qid = i / 2;
        if (qid > HV_NVME_QUEUES || !(n->csts & 1)) return;
        if (i & 1) { /* a completion queue's head: entries up to it have been seen */
            if (n->cq[qid].base && (uint32_t)v < n->cq[qid].size) n->cq[qid].head = (uint32_t)v;
            update_irq(n);
        } else if (n->sq[qid].base && (uint32_t)v < n->sq[qid].size) {
            n->sq[qid].tail = (uint32_t)v;
            run(n, (int)qid);
        }
        return;
    }
    switch (off) {
    case 0x0c: n->intms |= (uint32_t)v; update_irq(n); break;
    case 0x10: n->intms &= ~(uint32_t)v; update_irq(n); break;
    case 0x14: {
        uint32_t old = n->cc;
        n->cc = (uint32_t)v;
        if ((old ^ n->cc) & 1) enable(n, n->cc & 1);
        if ((n->cc >> 14) & 3) { /* shutdown notification: everything is on the disk file already */
            hv_disk_flush(n->disk);
            n->csts |= 2u << 2;
        } else n->csts &= ~(3u << 2);
        break;
    }
    case 0x24: n->aqa = (uint32_t)v; break;
    case 0x28:
        if (size >= 8) n->asq = v;
        else n->asq = (n->asq & ~0xffffffffull) | (uint32_t)v;
        break;
    case 0x2c: n->asq = (n->asq & 0xffffffffull) | v << 32; break;
    case 0x30:
        if (size >= 8) n->acq = v;
        else n->acq = (n->acq & ~0xffffffffull) | (uint32_t)v;
        break;
    case 0x34: n->acq = (n->acq & 0xffffffffull) | v << 32; break;
    default: break;
    }
}

HvNvme *hv_nvme_create(HvMachine *m, HvPcie *bus, HvDisk *disk) {
    HvNvme *n = calloc(1, sizeof *n);
    if (!n || !(n->bounce = malloc(BOUNCE))) { free(n); return NULL; }
    n->m = m;
    n->disk = disk;
    /* Queues of up to 1024 entries, contiguous; 7.5 s to become ready; the NVM command set; pages from 4 KB to 64 KB. */
    n->cap = 1023 | 1ull << 16 | 15ull << 24 | 1ull << 37 | 4ull << 52;
    hv_pci_init(&n->pci, 0x1b36, 0x0010, 0x01080202, 0x1af4, 0x1100); /* QEMU's NVMe ids; class: NVM Express */
    n->pci.opaque = n;
    n->pci.bar_read = bar_read;
    n->pci.bar_write = bar_write;
    hv_pci_add_bar(&n->pci, 0, BAR_SIZE, 1);
    if (hv_pcie_attach(bus, &n->pci) < 0) {
        hv_nvme_destroy(n);
        return NULL;
    }
    return n;
}

void hv_nvme_destroy(HvNvme *n) {
    if (!n) return;
    free(n->bounce);
    free(n);
}
