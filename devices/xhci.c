#include "xhci.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "machine.h"

/* Register layout in BAR 0. */
#define CAPLENGTH 0x40
#define OP 0x40          /* operational registers */
#define PORTREGS 0x440   /* OP + 0x400 */
#define RT 0x1000        /* runtime registers */
#define DB 0x2000        /* doorbells */
#define XECP 0x800       /* extended capabilities */
#define BAR_SIZE 0x4000

/* TRB types. */
enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6, TRB_EVENT_DATA = 7, TRB_NOOP = 8,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12, TRB_EVALUATE_CTX = 13,
    TRB_RESET_EP = 14, TRB_STOP_EP = 15, TRB_SET_TR_DEQUEUE = 16, TRB_RESET_DEVICE = 17, TRB_NOOP_CMD = 23,
    TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33, TRB_PORT_STATUS = 34,
};
/* Completion codes. */
enum { CC_SUCCESS = 1, CC_TRB_ERROR = 5, CC_STALL = 6, CC_SLOTS_FULL = 9, CC_SLOT_NOT_ENABLED = 11, CC_SHORT_PACKET = 13, CC_CONTEXT_STATE = 19 };

/* PORTSC bits. */
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR (1u << 4)
#define PORT_PLS_SHIFT 5
#define PORT_PP (1u << 9)
#define PORT_SPEED_SHIFT 10
#define PORT_LWS (1u << 16)
#define PORT_CSC (1u << 17)
#define PORT_PEC (1u << 18)
#define PORT_WRC (1u << 19)
#define PORT_OCC (1u << 20)
#define PORT_PRC (1u << 21)
#define PORT_PLC (1u << 22)
#define PORT_CEC (1u << 23)
#define PORT_CHANGES (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC | PORT_PLC | PORT_CEC)
#define SPEED_FULL 1
#define SPEED_HIGH 2

/* ---- the devices' descriptors ---- */

/* Port 1: a USB HID 1.11 boot keyboard. */

static const uint8_t kbd_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};

static const uint8_t kbd_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, /* USB 2.0, class per interface, EP0 64 bytes */
    0xac, 0x05, 0x20, 0x02,         /* vendor Apple, product 0x0220 (an ANSI keyboard): macOS knows its layout, no Keyboard Setup Assistant */
    0x00, 0x01, 1, 2, 3, 1,         /* bcdDevice 1.00, strings 1-3, one configuration */
};

static const uint8_t kbd_config_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,           /* one interface, remote wakeup, 100 mA */
    9, 4, 0, 0, 1, 3, 1, 1, 0,                /* interface 0: HID, boot, keyboard */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof kbd_report_desc, 0, /* HID 1.11, one report descriptor */
    7, 5, 0x81, 3, 8, 0, 10,                  /* EP1 IN, interrupt, 8 bytes, every 10 ms */
};

static const char *const kbd_strings[] = {NULL, "HengeVM", "HengeVM USB Keyboard", "1"};

/* Port 2: an Apple Magic Trackpad 2 (vendor 0x05AC, product 0x0265). The report
   descriptor is the real device's: a boot-mouse face (report ID 2), a 16-byte
   vendor data channel (report 0x3f) and a 1387-byte vendor blob (report 0x44).
   The multitouch report is NOT declared anywhere: after the guest sends
   feature report {2, 1}, the pad simply starts sending report ID 2 with a
   12-byte prefix and nine bytes a finger, which the guest's driver knows by
   the device ID. (The Linux kernel's hid-magicmouse does the same: it
   registers the MT2's report ID 2 by hand, because the descriptor omits it.) */

static const uint8_t tp_report_desc[] = {
    /* The mouse face: what the pad looks like before the guest switches it to multitouch. */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,       /* usage page (generic desktop), usage (mouse), collection (application) */
    0x09, 0x01, 0xa1, 0x00,                  /* usage (pointer), collection (physical) */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, /* buttons 1-3 */
    0x85, 0x02,                              /* report ID 2 */
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, /* X, Y, -127..127 */
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0x95, 0x04, 0x75, 0x08, 0x81, 0x01,
    0xc0, 0xc0,
    /* A 16-byte vendor data channel (report 0x3f). */
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01,       /* usage page (digitizer), usage (touch pad), collection (application) */
    0x06, 0x00, 0xff, 0x09, 0x0c,             /* vendor page 0xff00, usage 0x0c */
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x10,
    0x85, 0x3f,
    0x81, 0x22,                               /* data, variable, volatile */
    0xc0,
    /* A 1387-byte vendor blob (report 0x44). */
    0x06, 0x00, 0xff, 0x09, 0x0c,             /* vendor page 0xff00, usage 0x0c */
    0xa1, 0x01,                               /* collection (application) */
    0x06, 0x00, 0xff, 0x09, 0x0c,             /* usage 0x0c */
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x85, 0x44,
    0x75, 0x08, 0x96, 0x6b, 0x05,             /* 1387 bytes */
    0x81, 0x00,                               /* constant */
    0xc0,
};

static const uint8_t tp_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, /* USB 2.0, class per interface, EP0 64 bytes */
    0xac, 0x05, 0x65, 0x02,         /* vendor Apple, product 0x0265 (Magic Trackpad 2) */
    0x20, 0x02, 1, 2, 3, 1,         /* bcdDevice 2.20, strings 1-3, one configuration */
};

static const uint8_t tp_config_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,           /* one interface, remote wakeup, 100 mA */
    9, 4, 0, 0, 1, 3, 2, 1, 0,                /* interface 0: HID, boot, mouse */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof tp_report_desc, 0, /* HID 1.11, one report descriptor */
    7, 5, 0x81, 3, 64, 0, 4,                  /* EP1 IN, interrupt, 64 bytes, every 4 ms */
};

static const char *const tp_strings[] = {NULL, "Apple Inc.", "Magic Trackpad 2", "HengeVM"};

/* The UEFI board's pointer: a tablet, QEMU's usb-tablet report for report (three buttons, X and Y as
   0..32767 across the display, a wheel), which Windows and Linux take as an absolute pointer. */

static const uint8_t tab_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, /* buttons 1-3 */
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f, /* X, Y: 0..32767 */
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, /* wheel */
    0xc0, 0xc0,
};

static const uint8_t tab_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0x27, 0x06, 0x01, 0x00,         /* QEMU's tablet: vendor 0x0627, product 1 */
    0x00, 0x01, 1, 2, 3, 1,
};

static const uint8_t tab_config_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,
    9, 4, 0, 0, 1, 3, 0, 0, 0,                /* interface 0: HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof tab_report_desc, 0,
    7, 5, 0x81, 3, 8, 0, 10,                  /* EP1 IN, interrupt, 8 bytes, every 10 ms */
};

static const char *const tab_strings[] = {NULL, "HengeVM", "HengeVM USB Tablet", "1"};

/* The CD-ROM drive: USB mass storage, SCSI commands over the bulk-only transport. */

static const uint8_t cd_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0xf4, 0x46, 0x01, 0x00,         /* QEMU's storage device: vendor 0x46f4, product 1 */
    0x00, 0x01, 1, 2, 3, 1,
};

static const uint8_t cd_config_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,             /* interface 0: mass storage, SCSI, bulk-only */
    7, 5, 0x81, 2, 0x00, 0x02, 0,             /* EP1 IN, bulk, 512 bytes */
    7, 5, 0x02, 2, 0x00, 0x02, 0,             /* EP2 OUT, bulk, 512 bytes */
};

static const char *const cd_strings[] = {NULL, "HengeVM", "HengeVM USB CD-ROM", "1"};

struct HvXhciDev {
    const char *const *strings;
    const uint8_t *device_desc;
    const uint8_t *config_desc;
    unsigned config_desc_len;
    const uint8_t *report_desc;
    unsigned report_desc_len;
};
/* By HV_USB_* kind. */
static const struct HvXhciDev devs[] = {
    {kbd_strings, kbd_device_desc, kbd_config_desc, sizeof kbd_config_desc, kbd_report_desc, sizeof kbd_report_desc},
    {tp_strings, tp_device_desc, tp_config_desc, sizeof tp_config_desc, tp_report_desc, sizeof tp_report_desc},
    {tab_strings, tab_device_desc, tab_config_desc, sizeof tab_config_desc, tab_report_desc, sizeof tab_report_desc},
    {cd_strings, cd_device_desc, cd_config_desc, sizeof cd_config_desc, NULL, 0},
};

static int xlog(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_XHCI_LOG") != NULL;
    return on;
}

/* ---- guest memory ---- */

static void rd(HvXhci *x, uint64_t pa, void *buf, unsigned len) {
    if (hv_phys_read(x->m, pa, buf, len) != 0) memset(buf, 0, len);
}

static void wr(HvXhci *x, uint64_t pa, const void *buf, unsigned len) {
    hv_phys_write(x->m, pa, buf, len);
}

static uint32_t rd32(HvXhci *x, uint64_t pa) {
    uint32_t v;
    rd(x, pa, &v, 4);
    return v;
}

/* ---- interrupts and the event ring ---- */

static void update_irq(HvXhci *x) {
    int level = 0;
    for (int i = 0; i < HV_XHCI_INTRS; i++) level |= (x->intr[i].iman & 1) && (x->intr[i].iman & 2); /* IP, IE */
    hv_pci_set_intx(&x->pci, level && (x->usbcmd & 4)); /* INTE */
}

static void load_segment(HvXhci *x, struct HvXhciIntr *it) {
    uint64_t entry[2];
    rd(x, it->erstba + 16ull * it->seg_index, entry, 16);
    it->seg_base = entry[0] & ~0x3full;
    it->seg_size = (uint32_t)entry[1] & 0xffff;
    it->index = 0;
}

static void event_to(HvXhci *x, int target, uint64_t param, uint32_t status, uint32_t control) {
    struct HvXhciIntr *it = &x->intr[target < HV_XHCI_INTRS ? target : 0];
    if (!it->erstsz || !it->seg_size || !(x->usbcmd & 1)) return; /* a stopped controller writes nothing (port changes are sent again when it runs) */
    uint32_t trb[4] = {(uint32_t)param, (uint32_t)(param >> 32), status, (control & ~1u) | (uint32_t)it->pcs};
    /* The word with the cycle bit goes last, and whole: the driver may be reading the ring on another CPU
       at this moment, and the bit is what tells it the entry is there. An event seen with the parameter of
       whatever was in the ring before is a wild pointer in the driver's hands (Windows keeps its own in an
       Event Data TRB's: it stopped with KERNEL_MODE_HEAP_CORRUPTION now and then). */
    uint64_t at = it->seg_base + 16ull * it->index;
    uint32_t *ctl = hv_phys_ptr(x->m, at + 12, 4);
    if (!ctl) return;
    wr(x, at, trb, 12);
    __atomic_store_n(ctl, trb[3], __ATOMIC_RELEASE);
    if (++it->index == it->seg_size) { /* next segment, or wrap and flip the cycle */
        if (++it->seg_index >= it->erstsz) {
            it->seg_index = 0;
            it->pcs ^= 1;
        }
        load_segment(x, it);
    }
    it->iman |= 1;        /* IP */
    x->usbsts |= 1u << 3; /* EINT */
    update_irq(x);
}

static void event(HvXhci *x, uint64_t param, uint32_t status, uint32_t control) {
    event_to(x, 0, param, status, control);
}

static void port_event(HvXhci *x, int port) {
    event(x, (uint64_t)(port + 1) << 24, (uint32_t)CC_SUCCESS << 24, TRB_PORT_STATUS << 10);
}

static void cmd_completion(HvXhci *x, uint64_t trb_pa, int cc, int slot) {
    event(x, trb_pa, (uint32_t)cc << 24, TRB_CMD_COMPLETION << 10 | (uint32_t)slot << 24);
}

static void transfer_event(HvXhci *x, uint64_t trb_pa, int cc, uint32_t residual, int slot, int dci) {
    uint32_t dw2 = rd32(x, trb_pa + 8); /* each transfer TRB names its interrupter (bits 31:22) */
    event_to(x, (int)(dw2 >> 22), trb_pa, (uint32_t)cc << 24 | (residual & 0xffffff), TRB_TRANSFER_EVENT << 10 | (uint32_t)dci << 16 | (uint32_t)slot << 24);
}

/* ---- rings ---- */

/* Next TRB the guest owns on a ring, following links; 0 if the ring is empty. */
static int ring_peek(HvXhci *x, HvXhciRing *r, uint32_t trb[4], uint64_t *at) {
    for (int guard = 0; guard < 64; guard++) {
        /* The guest writes the TRB, then its cycle bit, from another core:
           read the control word first (acquire), then the rest. */
        uint32_t *ctl = hv_phys_ptr(x->m, r->dequeue + 12, 4);
        uint32_t c = ctl ? __atomic_load_n(ctl, __ATOMIC_ACQUIRE) : 0;
        if ((c & 1) != (uint32_t)r->ccs) return 0;
        rd(x, r->dequeue, trb, 12);
        trb[3] = c;
        if (((trb[3] >> 10) & 0x3f) == TRB_LINK) {
            if (trb[3] & 2) r->ccs ^= 1; /* toggle cycle */
            r->dequeue = ((uint64_t)trb[1] << 32 | trb[0]) & ~0xfull;
            continue;
        }
        *at = r->dequeue;
        return 1;
    }
    return 0;
}

static void ring_advance(HvXhciRing *r) {
    r->dequeue += 16;
}

/* ---- device contexts (32 bytes each; the output context is at DCBAA[slot]) ---- */

static uint64_t out_ctx(HvXhci *x, int slot) {
    uint64_t p;
    rd(x, x->dcbaap + 8ull * slot, &p, 8);
    return p & ~0x3full;
}

static void set_ep_state(HvXhci *x, int slot, int dci, uint32_t state) {
    uint64_t ctx = out_ctx(x, slot) + 32ull * dci;
    uint32_t dw0 = rd32(x, ctx);
    dw0 = (dw0 & ~7u) | state;
    wr(x, ctx, &dw0, 4);
}

/* ---- endpoint 0: the devices' control requests ---- */

static int tp_build_report(HvXhci *x, uint8_t *out);

/* The Magic Trackpad 2's surface, in 0.01 mm (160.0 x 114.9 mm). The guest's driver
   reads it out of the sensor-surface register to scale touches into millimetres. */
#define HV_TP_PHYS_X 16000
#define HV_TP_PHYS_Y 11490

/* Before the guest's multitouch driver will use the pad it reads a handful of
   sensor-configuration registers: it selects one by writing feature report 1, reads
   report 1 back for a five-byte header giving the register's length, then reads the
   register itself. These are the Magic Trackpad 2's answers, as VoodooInput's
   simulator gives them (the I2C trackpad emulators talk the same protocol). Returns
   the register's bytes, report ID first, or 0 for one we do not know. */
static int tp_register(int rid, uint8_t *out) {
    uint8_t wl = HV_TP_PHYS_X & 0xff, wh = (HV_TP_PHYS_X >> 8) & 0xff;
    uint8_t hl = HV_TP_PHYS_Y & 0xff, hh = (HV_TP_PHYS_Y >> 8) & 0xff;
    static const uint8_t d1[] = {0xd1, 0x81}; /* family ID */
    static const uint8_t d3[] = {0xd3, 0x01, 0x16, 0x1e, 0x03, 0x95, 0x00, 0x14, 0x1e, 0x62, 0x05, 0x00, 0x00}; /* sensor rows/columns */
    static const uint8_t d0[] = {0xd0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1e, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0e, 0x02, 0x00}; /* sensor region */
    static const uint8_t a1[] = {0xa1, 0x00, 0x00, 0x05, 0x00, 0xfc, 0x01}; /* sensor region param */
    static const uint8_t s7f[] = {0x7f, 0x00, 0x00, 0x00, 0x00};
    switch (rid) {
    case 0xd1: memcpy(out, d1, sizeof d1); return (int)sizeof d1;
    case 0xd3: memcpy(out, d3, sizeof d3); return (int)sizeof d3;
    case 0xd0: memcpy(out, d0, sizeof d0); return (int)sizeof d0;
    case 0xa1: memcpy(out, a1, sizeof a1); return (int)sizeof a1;
    case 0x7f: memcpy(out, s7f, sizeof s7f); return (int)sizeof s7f;
    case 0xc8: out[0] = 0xc8; out[1] = 0x08; return 2;
    case 0xd9: { /* sensor surface: width and height, then a fixed calibration tail */
        const uint8_t d9[] = {0xd9, wl, wh, 0, 0, hl, hh, 0, 0, 0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26};
        memcpy(out, d9, sizeof d9); return (int)sizeof d9;
    }
    case 0xdb: { /* the whole sensor description: every register, length-prefixed */
        const uint8_t db[] = {
            0xdb, 0x01, 0x02, 0x00,
            0xd1, 0x81, 0x0d, 0x00,
            0xd3, 0x01, 0x16, 0x1e, 0x03, 0x95, 0x00, 0x14, 0x1e, 0x62, 0x05, 0x00, 0x00, 0x10, 0x00,
            0xd0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1e, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0e, 0x02, 0x00, 0x07, 0x00,
            0xa1, 0x00, 0x00, 0x05, 0x00, 0xfc, 0x01, 0x11, 0x00,
            0xd9, wl, wh, 0, 0, hl, hh, 0, 0, 0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26,
            0x7f, 0x00, 0x00, 0x00, 0x00,
        };
        memcpy(out, db, sizeof db); return (int)sizeof db;
    }
    default: return 0;
    }
}

/* Feature report 1 was written with a register ID: remember the five-byte header the
   guest reads back next, telling it how long that register's answer is. */
static void tp_select(HvXhci *x, int reg) {
    uint8_t scratch[80];
    int n = tp_register(reg, scratch);
    if (n <= 0) { x->tp_f1_len = 0; return; }
    x->tp_f1[0] = 0x01;
    x->tp_f1[1] = (uint8_t)reg;
    x->tp_f1[2] = 0x00;
    x->tp_f1[3] = (uint8_t)((n - 1) & 0xff);
    x->tp_f1[4] = (uint8_t)(((n - 1) >> 8) & 0xff);
    x->tp_f1_len = 5;
}

/* Answer a SETUP on endpoint 0. Returns the reply length (data to send for IN
   requests), or -1 to stall. */
static int control(HvXhci *x, int slot, const uint8_t setup[8], uint8_t *out, int max) {
    int kind = x->kind[x->slots[slot].dev];
    const struct HvXhciDev *d = &devs[kind];
    uint8_t type = setup[0], req = setup[1];
    uint16_t value = (uint16_t)(setup[2] | setup[3] << 8);
    if (xlog()) fprintf(stderr, "xhci: request %02x %02x value %04x len %u\n", type, req, value, setup[6] | setup[7] << 8);
    /* Every request the trackpad gets goes to the log, unconditionally: the guest's
       driver handshake is exactly what we are trying to watch. Capped so a polling
       driver cannot flood it. */
    if (kind == HV_USB_TRACKPAD && x->tp_reqs < 80) {
        x->tp_reqs++;
        fprintf(stderr, "hengevm: xhci: tp request %02x %02x value %04x index %04x len %u\n",
                type, req, value, setup[4] | setup[5] << 8, setup[6] | setup[7] << 8);
    }
    const uint8_t *src = NULL;
    int len = 0;
    uint8_t tmp[HV_TP_REPORT_LEN + 8];
    if (type == 0x80 && req == 6) { /* GET_DESCRIPTOR (device) */
        uint8_t dt = value >> 8, idx = value & 0xff;
        if (dt == 1) src = d->device_desc, len = 18;
        else if (dt == 2) src = d->config_desc, len = (int)d->config_desc_len;
        else if (dt == 3) {
            if (idx == 0) {
                tmp[0] = 4, tmp[1] = 3, tmp[2] = 0x09, tmp[3] = 0x04; /* English (US) */
                src = tmp, len = 4;
            } else if (idx < 4) {
                const char *s = d->strings[idx];
                int n = (int)strlen(s);
                tmp[0] = (uint8_t)(2 + 2 * n), tmp[1] = 3;
                for (int i = 0; i < n; i++) tmp[2 + 2 * i] = (uint8_t)s[i], tmp[3 + 2 * i] = 0;
                src = tmp, len = 2 + 2 * n;
            } else
                return -1;
        } else
            return -1; /* no device qualifier: full speed only */
    } else if (type == 0x81 && req == 6) { /* GET_DESCRIPTOR (interface): HID, report */
        if (!d->report_desc) return -1;
        if ((value >> 8) == 0x22) src = d->report_desc, len = (int)d->report_desc_len;
        else if ((value >> 8) == 0x21) src = d->config_desc + 18, len = 9;
        else return -1;
    } else if (type == 0xa1 && req == 0xfe && kind == HV_USB_CDROM) { /* GET_MAX_LUN: one unit */
        tmp[0] = 0;
        src = tmp, len = 1;
    } else if (type == 0x80 && req == 0) { /* GET_STATUS */
        tmp[0] = 0, tmp[1] = 0;
        src = tmp, len = 2;
    } else if ((type == 0x81 || type == 0x82) && req == 0) {
        tmp[0] = 0, tmp[1] = 0;
        src = tmp, len = 2;
    } else if (type == 0x80 && req == 8) { /* GET_CONFIGURATION */
        tmp[0] = (uint8_t)x->slots[slot].configured;
        src = tmp, len = 1;
    } else if (type == 0x00 && req == 9) { /* SET_CONFIGURATION */
        x->slots[slot].configured = value & 0xff;
        return 0;
    } else if (type == 0x81 && req == 10) { /* GET_INTERFACE */
        tmp[0] = 0;
        src = tmp, len = 1;
    } else if (type == 0xa1 && req == 1) { /* GET_REPORT: wValue is (type << 8) | report ID */
        if (kind == HV_USB_KEYBOARD) src = x->state, len = 8;
        else if (kind == HV_USB_TABLET) src = x->tab_state, len = 6;
        else if (kind != HV_USB_TRACKPAD) return -1;
        else {
            int rtype = value >> 8, rid = value & 0xff;
            if (rtype == 3) { /* feature: the sensor-configuration registers */
                if (rid == 0x01) { /* the header for the register just selected */
                    if (!x->tp_f1_len) return -1;
                    src = x->tp_f1;
                    len = x->tp_f1_len;
                } else if (rid == 0x00) {
                    tmp[0] = 0x00; tmp[1] = 0x01;
                    src = tmp; len = 2;
                } else if (rid == 0x02) { /* multitouch mode: on */
                    tmp[0] = 0x02; tmp[1] = 0x01;
                    src = tmp; len = 2;
                } else {
                    len = tp_register(rid, tmp);
                    if (!len) return -1;
                    src = tmp;
                }
            } else if (rid == 0x02) { /* input: the current touch state */
                pthread_mutex_lock(&x->mu);
                len = tp_build_report(x, tmp);
                pthread_mutex_unlock(&x->mu);
                src = tmp;
            } else if (rid == 0x3f) { /* the vendor data channel: nothing on it */
                memset(tmp, 0, 16);
                len = 16;
            } else { /* 0x44 is a constant report: zeros, as far as we are concerned */
                memset(out, 0, (size_t)max);
                return max;
            }
        }
    } else if (type == 0xa1 && (req == 2 || req == 3)) { /* GET_IDLE, GET_PROTOCOL */
        tmp[0] = req == 3 ? 1 : 0;
        src = tmp, len = 1;
    } else if ((type & 0x60) == 0x20 || (type == 0x00 && (req == 1 || req == 3)) || (type == 0x01 && req == 11) ||
               (type == 0x02 && (req == 1 || req == 3)) ||
               ((type & 0x60) == 0x40 && kind == HV_USB_TRACKPAD)) {
        /* SET_IDLE, SET_PROTOCOL, SET_REPORT (the trackpad's multitouch switch and friends): accepted.
           Vendor requests to the trackpad too: stalling one may be what stops the guest's
           driver handshake cold, and we cannot tell what it wants yet. */
        return 0;
    } else {
        if (x->m->log) fprintf(stderr, "hengevm: xhci: unhandled request %02x %02x value %04x (slot %d, device %d)\n",
                               type, req, value, slot, x->slots[slot].dev);
        return -1;
    }
    if (len > max) len = max;
    memcpy(out, src, (size_t)len);
    return len;
}

/* Run the control transfers queued on the device's endpoint 0. */
static void run_ep0(HvXhci *x, int slot) {
    HvXhciRing *r = &x->slots[slot].ep[1];
    uint32_t trb[4];
    uint64_t at;
    uint8_t setup[8], reply[256];
    int reply_len = 0, stalled = 0;
    while (ring_peek(x, r, trb, &at)) {
        int type = (trb[3] >> 10) & 0x3f;
        int ioc = (trb[3] >> 5) & 1;
        if (type == TRB_SETUP) {
            memcpy(setup, trb, 8);
            uint16_t wlen = (uint16_t)(setup[6] | setup[7] << 8);
            reply_len = control(x, slot, setup, reply, wlen < sizeof reply ? wlen : (int)sizeof reply);
            stalled = reply_len < 0;
            ring_advance(r);
            if (stalled) {
                transfer_event(x, at, CC_STALL, 0, slot, 1);
                set_ep_state(x, slot, 1, 2); /* halted */
                x->slots[slot].ep_running[1] = 0;
                return;
            }
            if (ioc) transfer_event(x, at, CC_SUCCESS, 0, slot, 1);
        } else if (type == TRB_DATA) {
            uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
            uint32_t want = trb[2] & 0x1ffff;
            int in = (trb[3] >> 16) & 1;
            uint32_t n = 0;
            if (in) {
                n = reply_len < 0 ? 0 : (uint32_t)reply_len < want ? (uint32_t)reply_len : want;
                if (n) wr(x, buf, reply, n);
            } else {
                n = want; /* SET_REPORT and friends: take the data and ignore it */
                if (x->slots[slot].dev == 1 && want >= 2) {
                    uint8_t data[2];
                    rd(x, buf, data, 2); /* the data lives in guest memory, at the TRB's pointer */
                    if (data[0] == 0x02 && (data[1] & 1)) {
                        x->tp_mt = 1; /* the guest switched the pad to multitouch */
                        fprintf(stderr, "hengevm: xhci: trackpad: multitouch on\n");
                    } else if (data[0] == 0x01) {
                        tp_select(x, data[1]); /* the guest selected a sensor register to read */
                        fprintf(stderr, "hengevm: xhci: trackpad: select register %02x (%d bytes)\n", data[1], x->tp_f1_len ? x->tp_f1[3] : 0);
                    } else {
                        fprintf(stderr, "hengevm: xhci: trackpad: set report %02x %02x\n", data[0], data[1]);
                    }
                }
            }
            ring_advance(r);
            int isp = (trb[3] >> 2) & 1;
            if (ioc || (isp && n < want)) transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, 1);
        } else if (type == TRB_STATUS) {
            ring_advance(r);
            if (ioc) transfer_event(x, at, CC_SUCCESS, 0, slot, 1);
        } else {
            ring_advance(r);
            if (ioc) transfer_event(x, at, CC_SUCCESS, 0, slot, 1);
        }
    }
}

/* Give the device's waiting reports to the interrupt IN endpoint's posted transfers. */
static void run_ep_in(HvXhci *x, int slot) {
    struct HvXhciSlot *s = &x->slots[slot];
    int dci = -1;
    for (int d = 2; d < HV_XHCI_EPS; d++) if (s->ep_running[d]) { dci = d; break; }
    if (s->dev < 0 || dci < 0) return;
    HvXhciRing *r = &s->ep[dci];
    uint32_t trb[4];
    uint64_t at;
    pthread_mutex_lock(&x->mu);
    if (s->dev == 0) {
        while (x->rhead != x->rtail && ring_peek(x, r, trb, &at)) {
            int type = (trb[3] >> 10) & 0x3f;
            ring_advance(r);
            if (type != TRB_NORMAL) continue;
            uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
            uint32_t want = trb[2] & 0x1ffff;
            uint32_t n = want < 8 ? want : 8;
            wr(x, buf, x->reports[x->rhead % HV_XHCI_REPORTS], n);
            x->rhead++;
            x->delivered++;
            transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, dci);
        }
    } else {
        while (x->tp_rhead != x->tp_rtail && ring_peek(x, r, trb, &at)) {
            int type = (trb[3] >> 10) & 0x3f;
            ring_advance(r);
            if (type != TRB_NORMAL) continue;
            uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
            uint32_t want = trb[2] & 0x1ffff;
            uint32_t len = x->tp_rlen[x->tp_rhead % HV_TP_REPORTS];
            uint32_t n = want < len ? want : len;
            const uint8_t *rep = x->tp_reports[x->tp_rhead % HV_TP_REPORTS];
            wr(x, buf, rep, n);
            x->tp_rhead++;
            x->tp_delivered++;
            if (x->tp_delivered <= 16) fprintf(stderr, "hengevm: xhci: trackpad: %u bytes to the guest (delivered %llu)\n", n, (unsigned long long)x->tp_delivered);
            if (len >= 30 && x->tp_multi_logs < 40) { /* two fingers or more: dump the frame so we can watch them move */
                x->tp_multi_logs++;
                int nf = (int)((len - 12) / 9);
                fprintf(stderr, "hengevm: xhci: trackpad: %d fingers:", nf);
                for (int i = 0; i < nf; i++) {
                    const uint8_t *b = rep + 12 + 9 * i;
                    int xc = b[0] | ((b[1] & 0x1f) << 8); if (xc & 0x1000) xc -= 0x2000;
                    int yc = ((b[1] >> 5) & 0x7) | (b[2] << 3) | ((b[3] & 0x3) << 11); if (yc & 0x1000) yc -= 0x2000;
                    fprintf(stderr, " [id%d x%d y%d s%d]", b[8] & 0xf, xc, yc, (b[3] >> 5) & 7);
                }
                fprintf(stderr, "\n");
            }
            transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, dci);
        }
    }
    pthread_mutex_unlock(&x->mu);
}

/* ---- the UEFI board's devices: transfers a transfer descriptor at a time ----

   Windows' driver ends every transfer with an Event Data TRB and takes its results from that
   TRB's event (the bytes moved so far, not what is left), and both it and EDK2's spread one
   transfer's buffer over several chained TRBs. So these devices deal in whole TDs: the TRBs up
   to the first without the chain bit. */

#define TD_MAX_TRBS 1024

typedef struct Td {
    int n;
    uint64_t at[TD_MAX_TRBS];
    uint32_t trb[TD_MAX_TRBS][4];
    uint64_t total;     /* bytes its data TRBs carry */
    HvXhciRing end;     /* the ring after it */
} Td;

static int td_has_data(const uint32_t *trb) {
    int type = (trb[3] >> 10) & 0x3f;
    return type == TRB_NORMAL || type == TRB_DATA;
}

/* The next TD on a ring, left on it. 0: none there yet (or not all of it). */
static int td_peek(HvXhci *x, HvXhciRing *r, Td *td) {
    HvXhciRing c = *r;
    td->n = 0, td->total = 0;
    for (;;) {
        if (td->n == TD_MAX_TRBS || !ring_peek(x, &c, td->trb[td->n], &td->at[td->n])) return 0;
        const uint32_t *trb = td->trb[td->n++];
        if (td_has_data(trb)) td->total += trb[2] & 0x1ffff;
        ring_advance(&c);
        if (!(trb[3] & 0x10)) break; /* the chain ends here */
    }
    td->end = c;
    return 1;
}

/* A TD's buffers, in order: up to len bytes from buf into them, or out of them into buf. */
static void td_copy(HvXhci *x, Td *td, uint8_t *buf, uint64_t len, int to_guest) {
    for (int i = 0; i < td->n && len; i++) {
        uint32_t *trb = td->trb[i];
        if (!td_has_data(trb)) continue;
        uint64_t n = trb[2] & 0x1ffff;
        if (n > len) n = len;
        if (trb[3] & 0x40) { /* immediate data: the eight bytes of the TRB's own parameter */
            if (!to_guest) memcpy(buf, trb, n < 8 ? n : 8);
        } else {
            uint64_t pa = (uint64_t)trb[1] << 32 | trb[0];
            if (to_guest) wr(x, pa, buf, (unsigned)n);
            else rd(x, pa, buf, (unsigned)n);
        }
        buf += n, len -= n;
    }
}

/* A TD is done, having moved `done` bytes: the events its TRBs ask for, and on along the ring.
   A TRB that got less than its length ends the transfer there (a short packet): the data TRBs
   after it are passed over, and an Event Data TRB still reports. */
static void td_complete(HvXhci *x, int slot, int dci, HvXhciRing *r, Td *td, uint64_t done) {
    uint64_t left = done;
    uint32_t moved = 0; /* since the last Event Data TRB */
    int cc = CC_SUCCESS;
    for (int i = 0; i < td->n; i++) {
        const uint32_t *trb = td->trb[i];
        int type = (trb[3] >> 10) & 0x3f, ioc = (trb[3] >> 5) & 1, isp = (trb[3] >> 2) & 1;
        if (type == TRB_EVENT_DATA) {
            if (ioc)
                event_to(x, (int)(trb[2] >> 22), (uint64_t)trb[1] << 32 | trb[0], (uint32_t)cc << 24 | (moved & 0xffffff),
                         TRB_TRANSFER_EVENT << 10 | 1u << 2 | (uint32_t)dci << 16 | (uint32_t)slot << 24);
            moved = 0;
            continue;
        }
        if (cc != CC_SUCCESS) continue;
        uint32_t want = td_has_data(trb) ? trb[2] & 0x1ffff : 0;
        uint32_t n = left < want ? (uint32_t)left : want;
        left -= n, moved += n;
        if (n < want) {
            cc = CC_SHORT_PACKET;
            if (ioc || isp) transfer_event(x, td->at[i], cc, want - n, slot, dci);
        } else if (ioc) transfer_event(x, td->at[i], cc, 0, slot, dci);
    }
    *r = td->end;
}

static Td pc_td; /* device work is one thread's at a time (the device lock) */

/* Endpoint 0: a setup TD, a data TD if the request has data, a status TD. A request the device
   refuses stalls at the stage after its setup, as a device's would. */
static void pc_ep0(HvXhci *x, int slot) {
    struct HvXhciSlot *s = &x->slots[slot];
    HvXhciRing *r = &s->ep[1];
    Td *td = &pc_td;
    while (s->ep_running[1] && td_peek(x, r, td)) {
        int type = (td->trb[0][3] >> 10) & 0x3f;
        if (type == TRB_SETUP) {
            memcpy(s->setup, td->trb[0], 8);
            uint16_t wlen = (uint16_t)(s->setup[6] | s->setup[7] << 8);
            s->reply_len = control(x, slot, s->setup, s->reply, wlen < sizeof s->reply ? wlen : (int)sizeof s->reply);
            td_complete(x, slot, 1, r, td, 0);
            continue;
        }
        if ((type == TRB_DATA || type == TRB_STATUS) && s->reply_len < 0) {
            s->reply_len = 0;
            transfer_event(x, td->at[0], CC_STALL, 0, slot, 1);
            set_ep_state(x, slot, 1, 2); /* halted, until the driver resets it and moves the dequeue pointer on */
            s->ep_running[1] = 0;
            return;
        }
        uint64_t done = 0;
        if (type == TRB_DATA) {
            if ((td->trb[0][3] >> 16) & 1) { /* IN: the reply */
                done = (uint64_t)s->reply_len < td->total ? (uint64_t)s->reply_len : td->total;
                td_copy(x, td, s->reply, done, 1);
            } else done = td->total;       /* OUT (a keyboard's LEDs): taken and ignored */
        }
        td_complete(x, slot, 1, r, td, done);
    }
}

/* The keyboard's and the tablet's interrupt endpoint (EP1 IN): a waiting report for each TD posted. */
static void pc_hid_in(HvXhci *x, int slot) {
    struct HvXhciSlot *s = &x->slots[slot];
    enum { DCI = 3 };
    if (!s->ep_running[DCI]) return;
    int tablet = x->kind[s->dev] == HV_USB_TABLET;
    Td *td = &pc_td;
    pthread_mutex_lock(&x->mu);
    for (;;) {
        uint8_t *rep;
        uint64_t len;
        if (tablet) {
            if (x->tab_rhead == x->tab_rtail) break;
            rep = x->tab_reports[x->tab_rhead % HV_TAB_REPORTS], len = 6;
        } else {
            if (x->rhead == x->rtail) break;
            rep = x->reports[x->rhead % HV_XHCI_REPORTS], len = 8;
        }
        if (!td_peek(x, &s->ep[DCI], td)) break;
        if (len > td->total) len = td->total;
        td_copy(x, td, rep, len, 1);
        td_complete(x, slot, DCI, &s->ep[DCI], td, len);
        if (tablet) x->tab_rhead++;
        else x->rhead++, x->delivered++;
    }
    pthread_mutex_unlock(&x->mu);
}

/* ---- the CD-ROM drive ---- */

enum { MSC_COMMAND, MSC_DATA_IN, MSC_DATA_OUT, MSC_STATUS };
#define CD_BLOCK 2048u

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

static void cd_fail(HvXhci *x, uint8_t key, uint8_t asc) {
    x->msc_failed = 1;
    x->sense_key = key, x->sense_asc = asc;
    x->msc_len = 0;
}

/* `len` zeroed bytes for a command's answer. NULL (and the command failed) if there is no memory for it. */
static uint8_t *cd_reply(HvXhci *x, uint64_t len) {
    if (len > x->msc_cap) {
        uint8_t *nb = len <= (64u << 20) ? realloc(x->msc_buf, len) : NULL;
        if (!nb) {
            cd_fail(x, 5, 0x24);
            return NULL;
        }
        x->msc_buf = nb, x->msc_cap = len;
    }
    memset(x->msc_buf, 0, len);
    x->msc_len = len;
    return x->msc_buf;
}

static void cd_read(HvXhci *x, uint64_t lba, uint64_t count) {
    uint64_t blocks = x->cd->size / CD_BLOCK, len = count * CD_BLOCK;
    if (lba > blocks || count > blocks - lba) {
        cd_fail(x, 5, 0x21); /* logical block address out of range */
        return;
    }
    if (len > x->msc_expected) len = x->msc_expected; /* no more than the host will take */
    uint8_t *d = cd_reply(x, len);
    if (d && len && hv_disk_read(x->cd, lba * CD_BLOCK, d, len) != 0) cd_fail(x, 3, 0x11); /* unrecovered read error */
}

/* A SCSI command for the drive (MMC, as far as a disc that is only read needs it). */
static void cd_command(HvXhci *x, const uint8_t *cb) {
    uint32_t blocks = (uint32_t)(x->cd->size / CD_BLOCK);
    uint8_t *d;
    x->msc_failed = 0;
    x->msc_len = x->msc_off = 0;
    if (cb[0] != 0x03) x->sense_key = x->sense_asc = 0;
    switch (cb[0]) {
    case 0x00: case 0x1b: case 0x1e: case 0x35: break; /* test unit ready, start/stop, prevent removal, synchronize cache */
    case 0x03: /* request sense */
        if (!(d = cd_reply(x, 18))) break;
        d[0] = 0x70, d[2] = x->sense_key, d[7] = 10, d[12] = x->sense_asc;
        x->sense_key = x->sense_asc = 0;
        break;
    case 0x12: /* inquiry */
        if (cb[1] & 1) { /* no vital product data pages */
            cd_fail(x, 5, 0x24);
            break;
        }
        if (!(d = cd_reply(x, 36))) break;
        d[0] = 0x05, d[1] = 0x80, d[2] = 0x05, d[3] = 0x02, d[4] = 31; /* a CD-ROM drive, removable medium, SPC-3 */
        memcpy(d + 8, "HENGE   ", 8), memcpy(d + 16, "CD-ROM          ", 16), memcpy(d + 32, "1.0 ", 4);
        break;
    case 0x1a: /* mode sense (6): no pages, write-protected */
        if (!(d = cd_reply(x, 4))) break;
        d[0] = 3, d[2] = 0x80;
        break;
    case 0x5a: /* mode sense (10) */
        if (!(d = cd_reply(x, 8))) break;
        d[1] = 6, d[3] = 0x80;
        break;
    case 0x23: /* read format capacities */
        if (!(d = cd_reply(x, 12))) break;
        d[3] = 8;
        put_be32(d + 4, blocks);
        d[8] = 2, d[10] = CD_BLOCK >> 8; /* formatted medium, 2048-byte blocks */
        break;
    case 0x25: /* read capacity: the last block and the block size */
        if (!(d = cd_reply(x, 8))) break;
        put_be32(d, blocks ? blocks - 1 : 0);
        put_be32(d + 4, CD_BLOCK);
        break;
    case 0x28: cd_read(x, be32(cb + 2), (uint32_t)(cb[7] << 8 | cb[8])); break;
    case 0xa8: cd_read(x, be32(cb + 2), be32(cb + 6)); break;
    case 0x43: { /* read TOC: one session, one data track from block 0, then the lead-out */
        int format = cb[2] & 0xf ? cb[2] & 0xf : cb[9] >> 6, msf = cb[1] & 2;
        if (format == 1) {
            if (!(d = cd_reply(x, 12))) break;
            d[1] = 10, d[2] = d[3] = 1, d[5] = 0x14, d[6] = 1;
            break;
        }
        if (format != 0) {
            cd_fail(x, 5, 0x24);
            break;
        }
        if (!(d = cd_reply(x, 20))) break;
        d[1] = 18, d[2] = d[3] = 1;
        d[5] = 0x14, d[6] = 1;
        d[13] = 0x16, d[14] = 0xaa;
        if (msf) { /* minutes, seconds, frames: 75 frames a second, two seconds in */
            uint32_t f = blocks + 150;
            d[10] = 2;
            d[17] = (uint8_t)(f / 75 / 60), d[18] = (uint8_t)(f / 75 % 60), d[19] = (uint8_t)(f % 75);
        } else put_be32(d + 16, blocks);
        break;
    }
    case 0x46: /* get configuration: a DVD-ROM drive with a disc in it */
        if (!(d = cd_reply(x, 40))) break;
        put_be32(d, 36);
        put_be16(d + 6, 0x0010);
        d[10] = 0x03, d[11] = 8;                 /* feature 0, the profile list: DVD-ROM (current), CD-ROM */
        put_be16(d + 12, 0x0010), d[14] = 1;
        put_be16(d + 16, 0x0008);
        put_be16(d + 20, 0x0001), d[22] = 0x0b, d[23] = 8; /* feature 1, core: the physical interface is USB */
        put_be32(d + 24, 8), d[28] = 1;
        put_be16(d + 32, 0x0003), d[34] = 0x03, d[35] = 4; /* feature 3, removable medium: a tray, no lock */
        d[36] = 0x20;
        break;
    case 0x4a: /* get event status notification, polled: no news, a disc is in */
        if (!(cb[1] & 1)) {
            cd_fail(x, 5, 0x24);
            break;
        }
        if (!(d = cd_reply(x, 8))) break;
        d[3] = 0x10; /* the media class is the one supported */
        if (cb[4] & 0x10) d[1] = 4, d[2] = 4, d[5] = 0x02;
        else d[2] = 0x80; /* no event of the classes asked for */
        break;
    case 0x51: /* read disc information: complete, one session, one track */
        if (!(d = cd_reply(x, 34))) break;
        d[1] = 32, d[2] = 0x0e, d[3] = 1, d[4] = 1, d[5] = 1, d[6] = 1, d[7] = 0x20;
        break;
    case 0xa0: /* report LUNs: unit 0 */
        if (!(d = cd_reply(x, 16))) break;
        d[3] = 8;
        break;
    case 0xbd: cd_reply(x, 8); break; /* mechanism status: idle */
    case 0x2a: case 0xaa: case 0x2e: cd_fail(x, 7, 0x27); break; /* writes: write-protected */
    default:
        if (xlog()) fprintf(stderr, "xhci: cd: command %02x not known\n", cb[0]);
        cd_fail(x, 5, 0x20); /* invalid command */
        break;
    }
    if (xlog()) fprintf(stderr, "xhci: cd: command %02x (%02x %02x %02x %02x %02x) expects %u: %s, %llu bytes\n", cb[0], cb[1], cb[2], cb[3],
                        cb[4], cb[5], x->msc_expected, x->msc_failed ? "failed" : "ok", (unsigned long long)x->msc_len);
    if (x->msc_len > x->msc_expected) x->msc_len = x->msc_expected;
}

/* Bulk-only transport: a 31-byte command block on the OUT endpoint, the data either way, then a
   13-byte status block on the IN endpoint. The host may post the TD for a stage before the stage
   is reached: it stays on its ring until then. */
static void pc_cd(HvXhci *x, int slot) {
    struct HvXhciSlot *s = &x->slots[slot];
    enum { DCI_IN = 3, DCI_OUT = 4 };
    Td *td = &pc_td;
    for (;;) {
        if (x->msc_state == MSC_COMMAND) {
            if (!s->ep_running[DCI_OUT] || !td_peek(x, &s->ep[DCI_OUT], td)) return;
            uint8_t cbw[31] = {0};
            td_copy(x, td, cbw, td->total < sizeof cbw ? td->total : sizeof cbw, 0);
            td_complete(x, slot, DCI_OUT, &s->ep[DCI_OUT], td, td->total);
            if (td->total != sizeof cbw || memcmp(cbw, "USBC", 4)) continue; /* not a command block */
            memcpy(&x->msc_tag, cbw + 4, 4);
            memcpy(&x->msc_expected, cbw + 8, 4);
            x->msc_moved = 0;
            cd_command(x, cbw + 15);
            x->msc_state = !x->msc_expected ? MSC_STATUS : cbw[12] & 0x80 ? MSC_DATA_IN : MSC_DATA_OUT;
        } else if (x->msc_state == MSC_DATA_IN) {
            if (!s->ep_running[DCI_IN] || !td_peek(x, &s->ep[DCI_IN], td)) return;
            uint64_t n = x->msc_len - x->msc_off;
            if (n > td->total) n = td->total;
            td_copy(x, td, x->msc_buf + x->msc_off, n, 1);
            td_complete(x, slot, DCI_IN, &s->ep[DCI_IN], td, n); /* short, even empty, when there is no more */
            x->msc_off += n, x->msc_moved += (uint32_t)n;
            if (x->msc_off == x->msc_len) x->msc_state = MSC_STATUS;
        } else if (x->msc_state == MSC_DATA_OUT) { /* nothing here takes data: it is swallowed */
            if (!s->ep_running[DCI_OUT] || !td_peek(x, &s->ep[DCI_OUT], td)) return;
            td_complete(x, slot, DCI_OUT, &s->ep[DCI_OUT], td, td->total);
            x->msc_moved += (uint32_t)td->total;
            if (x->msc_moved >= x->msc_expected) x->msc_state = MSC_STATUS;
        } else {
            if (!s->ep_running[DCI_IN] || !td_peek(x, &s->ep[DCI_IN], td)) return;
            uint8_t csw[13] = "USBS";
            uint32_t residue = x->msc_expected > x->msc_moved ? x->msc_expected - x->msc_moved : 0;
            memcpy(csw + 4, &x->msc_tag, 4);
            memcpy(csw + 8, &residue, 4);
            csw[12] = x->msc_failed;
            uint64_t n = td->total < sizeof csw ? td->total : sizeof csw;
            td_copy(x, td, csw, n, 1);
            td_complete(x, slot, DCI_IN, &s->ep[DCI_IN], td, n);
            x->msc_state = MSC_COMMAND;
        }
    }
}

static void pc_run(HvXhci *x, int slot, int dci) {
    struct HvXhciSlot *s = &x->slots[slot];
    if (s->dev < 0) return;
    if (dci == 1) pc_ep0(x, slot);
    else if (x->kind[s->dev] == HV_USB_CDROM) pc_cd(x, slot);
    else pc_hid_in(x, slot);
}

/* ---- commands ---- */

static void init_ring_from_ctx(HvXhci *x, int slot, int dci) {
    uint64_t ctx = out_ctx(x, slot) + 32ull * dci;
    uint64_t deq;
    rd(x, ctx + 8, &deq, 8);
    x->slots[slot].ep[dci].dequeue = deq & ~0xfull;
    x->slots[slot].ep[dci].ccs = (int)(deq & 1);
    x->slots[slot].ep_running[dci] = 1;
}

static int address_device(HvXhci *x, int slot, uint64_t input, int bsr) {
    uint64_t out = out_ctx(x, slot);
    uint8_t slot_ctx[32], ep0[32];
    rd(x, input + 32, slot_ctx, 32);
    rd(x, input + 64, ep0, 32);
    int port = slot_ctx[6]; /* root hub port number, dword 1 bits 23:16 */
    int dev = port - 1;
    if (dev < 0 || dev >= x->nports) return CC_TRB_ERROR;
    x->slots[slot].dev = dev;
    uint32_t dw3 = (uint32_t)(bsr ? 1 : 2) << 27 | (uint32_t)(bsr ? 0 : slot); /* default or addressed; address = slot */
    memcpy(slot_ctx + 12, &dw3, 4);
    uint32_t e0 = 0;
    memcpy(&e0, ep0, 4);
    e0 = (e0 & ~7u) | 1; /* running */
    memcpy(ep0, &e0, 4);
    wr(x, out, slot_ctx, 32);
    wr(x, out + 32, ep0, 32);
    init_ring_from_ctx(x, slot, 1);
    return CC_SUCCESS;
}

static int configure_endpoints(HvXhci *x, int slot, uint64_t input, int deconfigure) {
    struct HvXhciSlot *s = &x->slots[slot];
    uint64_t out = out_ctx(x, slot);
    if (deconfigure) {
        for (int dci = 2; dci < HV_XHCI_EPS; dci++) s->ep_running[dci] = 0;
        return CC_SUCCESS;
    }
    uint32_t drop = rd32(x, input), add = rd32(x, input + 4);
    for (int dci = 2; dci < HV_XHCI_EPS; dci++) {
        if (drop & (1u << dci)) {
            s->ep_running[dci] = 0;
            uint8_t zero[32] = {0};
            wr(x, out + 32ull * dci, zero, 32);
        }
        if (add & (1u << dci)) {
            uint8_t ep[32];
            rd(x, input + 32ull * (dci + 1), ep, 32);
            uint32_t e0;
            memcpy(&e0, ep, 4);
            e0 = (e0 & ~7u) | 1;
            memcpy(ep, &e0, 4);
            wr(x, out + 32ull * dci, ep, 32);
            init_ring_from_ctx(x, slot, dci);
        }
    }
    if (add & 1) { /* slot context: context entries and friends */
        uint8_t slot_ctx[32];
        rd(x, input + 32, slot_ctx, 32);
        uint32_t dw3 = (uint32_t)3 << 27 | (uint32_t)slot; /* configured */
        memcpy(slot_ctx + 12, &dw3, 4);
        wr(x, out, slot_ctx, 32);
    } else {
        uint32_t dw3 = (uint32_t)3 << 27 | (uint32_t)slot;
        wr(x, out + 12, &dw3, 4);
    }
    return CC_SUCCESS;
}

static void run_commands(HvXhci *x) {
    uint32_t trb[4];
    uint64_t at;
    while (ring_peek(x, &x->cmd, trb, &at)) {
        ring_advance(&x->cmd);
        int type = (trb[3] >> 10) & 0x3f;
        int slot = (int)(trb[3] >> 24);
        uint64_t ptr = ((uint64_t)trb[1] << 32 | trb[0]) & ~0xfull;
        int cc = CC_SUCCESS;
        if (xlog()) fprintf(stderr, "xhci: command %d slot %d ptr %llx dw3 %08x\n", type, slot, (unsigned long long)ptr, trb[3]);
        /* The slot ID is the guest's to write (8 bits): every command but these two indexes x->slots with it. */
        if (type != TRB_ENABLE_SLOT && type != TRB_NOOP_CMD && (slot < 1 || slot > HV_XHCI_SLOTS)) {
            cmd_completion(x, at, CC_SLOT_NOT_ENABLED, slot);
            continue;
        }
        switch (type) {
        case TRB_ENABLE_SLOT: {
            int s = 1;
            while (s <= HV_XHCI_SLOTS && x->slot_used[s]) s++;
            if (s > HV_XHCI_SLOTS) {
                cmd_completion(x, at, CC_SLOTS_FULL, 0);
                continue;
            }
            x->slot_used[s] = 1;
            cmd_completion(x, at, CC_SUCCESS, s);
            continue;
        }
        case TRB_DISABLE_SLOT:
            if (slot >= 1 && slot <= HV_XHCI_SLOTS) {
                x->slot_used[slot] = 0;
                x->slots[slot].dev = -1;
                memset(x->slots[slot].ep_running, 0, sizeof x->slots[slot].ep_running);
            }
            break;
        case TRB_ADDRESS_DEVICE:
            cc = address_device(x, slot, ptr, (trb[3] >> 9) & 1);
            break;
        case TRB_CONFIGURE_EP:
            cc = configure_endpoints(x, slot, ptr, (trb[3] >> 9) & 1);
            break;
        case TRB_EVALUATE_CTX: { /* EP0 max packet size, slot fields: copy what was added */
            uint32_t add = rd32(x, ptr + 4);
            uint64_t out = out_ctx(x, slot);
            if (add & 2) {
                uint32_t dw1 = rd32(x, ptr + 64 + 4);
                wr(x, out + 32 + 4, &dw1, 4);
            }
            break;
        }
        case TRB_RESET_EP: {
            int dci = (trb[3] >> 16) & 0x1f;
            set_ep_state(x, slot, dci, 3); /* stopped */
            break;
        }
        case TRB_STOP_EP: {
            int dci = (trb[3] >> 16) & 0x1f;
            x->slots[slot].ep_running[dci] = 0;
            set_ep_state(x, slot, dci, 3);
            /* The dequeue pointer goes back into the context for the driver to read. */
            uint64_t deq = x->slots[slot].ep[dci].dequeue | (uint64_t)x->slots[slot].ep[dci].ccs;
            wr(x, out_ctx(x, slot) + 32ull * dci + 8, &deq, 8);
            break;
        }
        case TRB_SET_TR_DEQUEUE: {
            int dci = (trb[3] >> 16) & 0x1f;
            uint64_t p = (uint64_t)trb[1] << 32 | trb[0];
            x->slots[slot].ep[dci].dequeue = p & ~0xfull;
            x->slots[slot].ep[dci].ccs = (int)(p & 1);
            wr(x, out_ctx(x, slot) + 32ull * dci + 8, &p, 8);
            break;
        }
        case TRB_RESET_DEVICE: {
            for (int dci = 2; dci < HV_XHCI_EPS; dci++) x->slots[slot].ep_running[dci] = 0;
            uint32_t dw3 = (uint32_t)1 << 27; /* default state, address 0 */
            wr(x, out_ctx(x, slot) + 12, &dw3, 4);
            x->slots[slot].configured = 0;
            break;
        }
        case TRB_NOOP_CMD:
            break;
        default:
            if (x->m->log) fprintf(stderr, "hengevm: xhci: unhandled command TRB type %d\n", type);
            cc = CC_TRB_ERROR;
            break;
        }
        cmd_completion(x, at, cc, slot);
    }
}

/* ---- ports ---- */

/* Port 1 is the full-speed keyboard; port 2 is the high-speed trackpad, as the real
   Magic Trackpad 2 is. On the UEFI board the tablet is full-speed too, and the CD-ROM drive high-speed. */
static uint32_t port_speed(HvXhci *x, int p) {
    /* The port speed ids a controller without its own table has: 1 full, 2 low, 3 high. The virtual Mac's
       trackpad has always said 2, and macOS works it. */
    return x->kind[p] == HV_USB_CDROM ? 3 : x->kind[p] == HV_USB_TRACKPAD ? SPEED_HIGH : SPEED_FULL;
}

static void port_connect(HvXhci *x) {
    for (int p = 0; p < x->nports; p++) {
        x->portsc[p] = PORT_PP | PORT_CCS | PORT_CSC | port_speed(x, p) << PORT_SPEED_SHIFT | 7u << PORT_PLS_SHIFT;
        port_event(x, p);
    }
}

static void port_write(HvXhci *x, int p, uint32_t v) {
    if (xlog()) fprintf(stderr, "xhci: portsc write %08x (was %08x)\n", v, x->portsc[p]);
    uint32_t s = x->portsc[p];
    s &= ~(v & PORT_CHANGES); /* write 1 to clear */
    if (v & PORT_PED) s &= ~PORT_PED;
    if (v & PORT_PR) { /* reset: enabled, in U0 */
        s = (s & ~(0xfu << PORT_PLS_SHIFT)) | PORT_PED | PORT_PRC;
        x->portsc[p] = s;
        port_event(x, p);
        return;
    }
    if ((v & PORT_LWS) && (s & PORT_CCS)) {
        uint32_t pls = (v >> PORT_PLS_SHIFT) & 0xf;
        s = (s & ~(0xfu << PORT_PLS_SHIFT)) | pls << PORT_PLS_SHIFT;
        if (pls == 0 || pls == 15) { /* U0 or resume: report the link change */
            s = (s & ~(0xfu << PORT_PLS_SHIFT)) | PORT_PLC;
            x->portsc[p] = s;
            port_event(x, p);
            return;
        }
    }
    s = (s & ~PORT_PP) | (v & PORT_PP);
    x->portsc[p] = s;
}

/* ---- registers ---- */

static void reset(HvXhci *x) {
    x->usbcmd = 0;
    x->usbsts = 1; /* halted */
    x->dnctrl = x->config = 0;
    x->dcbaap = 0;
    memset(&x->cmd, 0, sizeof x->cmd);
    memset(x->intr, 0, sizeof x->intr);
    for (int i = 0; i < HV_XHCI_INTRS; i++) x->intr[i].pcs = 1;
    memset(x->slot_used, 0, sizeof x->slot_used);
    memset(x->slots, 0, sizeof x->slots);
    for (int s = 1; s <= HV_XHCI_SLOTS; s++) x->slots[s].dev = -1;
    memset(x->reports, 0, sizeof x->reports);
    x->rhead = x->rtail = 0;
    memset(x->state, 0, sizeof x->state);
    memset(x->tp_fingers, 0, sizeof x->tp_fingers);
    x->tp_button = 0;
    x->tp_ts = 0;
    x->tp_rhead = x->tp_rtail = 0;
    x->tp_mt = 0;
    x->tp_delivered = 0;
    x->tp_reqs = 0;
    x->tp_db = 0;
    x->tp_multi_logs = 0;
    x->tp_f1_len = 0;
    x->tab_rhead = x->tab_rtail = 0;
    x->msc_state = 0;
    x->msc_len = x->msc_off = 0;
    x->sense_key = x->sense_asc = 0;
    for (int p = 0; p < x->nports; p++)
        x->portsc[p] = PORT_PP | PORT_CCS | PORT_CSC | port_speed(x, p) << PORT_SPEED_SHIFT | 7u << PORT_PLS_SHIFT;
    update_irq(x);
}

static uint64_t bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvXhci *x = d->opaque;
    if (bar != 0) return 0;
    uint64_t v = 0;
    if (xlog()) {
        static int n;
        if (n++ < 60) fprintf(stderr, "xhci: read +%llx (%u)\n", (unsigned long long)off, size);
    }
    if (off < CAPLENGTH) {
        const uint32_t cap[8] = {
            CAPLENGTH | 0x0100u << 16,                           /* CAPLENGTH, HCIVERSION 1.0 */
            HV_XHCI_SLOTS | (uint32_t)HV_XHCI_INTRS << 8 | (uint32_t)x->nports << 24, /* slots, interrupters, ports */
            1u << 4,                                             /* ERST max 2 segments */
            0,
            1u | (uint32_t)(XECP / 4) << 16,                     /* 64-bit addressing, extended caps */
            DB,
            RT,
            0,
        };
        uint32_t i = (uint32_t)off / 4;
        v = cap[i];
        if (size == 8 && i + 1 < 8) v |= (uint64_t)cap[i + 1] << 32;
        v >>= 8 * (off & 3);
    } else if (off >= XECP && off < XECP + 16) { /* Supported Protocol: USB 2.0 on every port */
        /* Ports from 1, and how many (the virtual Mac's has always said 3 for its two). */
        const uint32_t ext[4] = {2u | 0x0200u << 16, 0x20425355, 1u | (x->pc ? (uint32_t)x->nports : 3u) << 8, 0};
        uint32_t i = (uint32_t)(off - XECP) / 4;
        v = ext[i];
        if (size == 8 && i + 1 < 4) v |= (uint64_t)ext[i + 1] << 32;
    } else if (off >= OP && off < PORTREGS) {
        switch (off - OP) {
        case 0x00: v = x->usbcmd; break;
        case 0x04: v = x->usbsts; break;
        case 0x08: v = 1; break; /* 4 KB pages */
        case 0x14: v = x->dnctrl; break;
        case 0x18: v = x->cmd_running ? 8 : 0; break; /* CRCR reads CRR only */
        case 0x1c: v = 0; break;
        case 0x30: v = size == 8 ? x->dcbaap : (uint32_t)x->dcbaap; break;
        case 0x34: v = x->dcbaap >> 32; break;
        case 0x38: v = x->config; break;
        }
    } else if (off >= PORTREGS && off < PORTREGS + 16u * (unsigned)x->nports) {
        int p = (int)(off - PORTREGS) / 16;
        v = (off - PORTREGS) % 16 == 0 ? x->portsc[p] : 0;
    } else if (off >= RT && off < RT + 0x20 + 32u * HV_XHCI_INTRS) {
        if (off - RT < 0x20) return 0; /* MFINDEX */
        struct HvXhciIntr *it = &x->intr[(off - RT - 0x20) / 32];
        switch ((off - RT - 0x20) % 32) {
        case 0x00: v = it->iman; break;
        case 0x04: v = it->imod; break;
        case 0x08: v = it->erstsz; break;
        case 0x10: v = size == 8 ? it->erstba : (uint32_t)it->erstba; break;
        case 0x14: v = it->erstba >> 32; break;
        case 0x18: v = it->erdp; break;
        case 0x1c: v = it->erdp >> 32; break;
        }
    }
    return v;
}

static void set64(uint64_t *f, uint64_t off_in_reg, uint64_t v, unsigned size) {
    if (size == 8) *f = v;
    else if (off_in_reg) *f = (*f & 0xffffffffull) | v << 32;
    else *f = (*f & ~0xffffffffull) | (uint32_t)v;
}

static void bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t v, unsigned size) {
    HvXhci *x = d->opaque;
    if (bar != 0) return;
    if (xlog() && off < DB) fprintf(stderr, "xhci: write +%llx = %llx (%u)\n", (unsigned long long)off, (unsigned long long)v, size);
    if (off >= OP && off < PORTREGS) {
        uint64_t r = off - OP;
        switch (r) {
        case 0x00:
            if (v & 2) { /* HCRST */
                reset(x);
                port_connect(x);
                x->usbsts |= 1;
                return;
            }
            if ((v & 1) && !(x->usbcmd & 1)) { /* run */
                x->usbcmd = (uint32_t)v;
                x->usbsts &= ~1u;
                for (int p = 0; p < x->nports; p++)
                    if (x->portsc[p] & PORT_CSC) port_event(x, p);
            } else if (!(v & 1)) {
                x->usbcmd = (uint32_t)v;
                x->usbsts |= 1;
            } else
                x->usbcmd = (uint32_t)v;
            update_irq(x);
            break;
        case 0x04:
            x->usbsts &= ~((uint32_t)v & (1u << 2 | 1u << 3 | 1u << 4 | 1u << 10)); /* HSE, EINT, PCD, SRE: write 1 to clear */
            break;
        case 0x14: x->dnctrl = (uint32_t)v; break;
        case 0x18:
        case 0x1c: {
            uint64_t crcr = 0;
            set64(&crcr, r - 0x18, v, size);
            if (r == 0x18 && size == 4) { /* low half: pointer low bits and RCS */
                x->cmd.dequeue = (x->cmd.dequeue & ~0xffffffffull) | ((uint32_t)v & ~0x3fu);
                x->cmd.ccs = (int)(v & 1);
            } else if (r == 0x1c) {
                x->cmd.dequeue = (x->cmd.dequeue & 0xffffffffull) | v << 32;
            } else {
                x->cmd.dequeue = v & ~0x3full;
                x->cmd.ccs = (int)(v & 1);
            }
            break;
        }
        case 0x30:
        case 0x34: set64(&x->dcbaap, r - 0x30, v, size); x->dcbaap &= ~0x3full; break;
        case 0x38: x->config = (uint32_t)v; break;
        }
    } else if (off >= PORTREGS && off < PORTREGS + 16u * (unsigned)x->nports) {
        if ((off - PORTREGS) % 16 == 0) port_write(x, (int)(off - PORTREGS) / 16, (uint32_t)v);
    } else if (off >= RT + 0x20 && off < RT + 0x20 + 32u * HV_XHCI_INTRS) {
        struct HvXhciIntr *it = &x->intr[(off - RT - 0x20) / 32];
        uint64_t r = (off - RT - 0x20) % 32;
        switch (r) {
        case 0x00:
            if (v & 1) it->iman &= ~1u; /* IP: write 1 to clear */
            it->iman = (it->iman & 1) | ((uint32_t)v & 2);
            update_irq(x);
            break;
        case 0x04: it->imod = (uint32_t)v; break;
        case 0x08: it->erstsz = (uint32_t)v & 0xffff; break;
        case 0x10:
        case 0x14:
            set64(&it->erstba, r - 0x10, v, size);
            if (r == 0x14 || size == 8) { /* the high half (or all of it) is written last */
                it->erstba &= ~0x3full;
                it->seg_index = 0;
                it->pcs = 1;
                load_segment(x, it);
            }
            break;
        case 0x18:
        case 0x1c:
            set64(&it->erdp, r - 0x18, v, size);
            if (r == 0x18 && (v & 8)) it->erdp &= ~8ull; /* EHB: write 1 to clear */
            break;
        }
    } else if (off >= DB && off < DB + 4u * (HV_XHCI_SLOTS + 1)) {
        int slot = (int)(off - DB) / 4;
        if (slot == 0) {
            x->cmd_running = 1;
            run_commands(x);
        } else if (slot <= HV_XHCI_SLOTS && x->slots[slot].dev >= 0) {
            int dci = (int)(v & 0xff);
            if (dci < 1 || dci >= HV_XHCI_EPS) return;
            if (x->kind[x->slots[slot].dev] == HV_USB_TRACKPAD && x->tp_db < 8) {
                x->tp_db++;
                fprintf(stderr, "hengevm: xhci: trackpad: doorbell dci %d\n", dci);
            }
            /* A doorbell starts a stopped endpoint again (a halted one waits for Reset Endpoint). */
            uint32_t state = rd32(x, out_ctx(x, slot) + 32ull * dci) & 7;
            if (state == 2) return;
            if (state != 1) set_ep_state(x, slot, dci, 1);
            x->slots[slot].ep_running[dci] = 1;
            if (x->pc) pc_run(x, slot, dci);
            else if (dci == 1) run_ep0(x, slot);
            else run_ep_in(x, slot);
        }
    }
}

/* ---- the keyboard's reports ---- */

void hv_xhci_key(HvXhci *x, uint8_t usage, int down) {
    if (!x || !usage) return;
    pthread_mutex_lock(&x->mu);
    uint8_t *s = x->state;
    if (usage >= 0xe0 && usage <= 0xe7) { /* modifiers live in byte 0 */
        uint8_t bit = (uint8_t)(1u << (usage - 0xe0));
        s[0] = down ? s[0] | bit : s[0] & ~bit;
    } else {
        int i;
        for (i = 2; i < 8 && s[i] != usage; i++) {}
        if (down && i == 8) { /* not held yet: take a free place */
            for (i = 2; i < 8 && s[i]; i++) {}
            if (i < 8) s[i] = usage;
        } else if (!down && i < 8) { /* let go: close the gap */
            memmove(s + i, s + i + 1, (size_t)(7 - i));
            s[7] = 0;
        }
    }
    if (x->rtail - x->rhead < HV_XHCI_REPORTS) memcpy(x->reports[x->rtail++ % HV_XHCI_REPORTS], s, 8);
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

/* Linux input key codes to HID keyboard usages (the same table macOS's virtio
   keyboard driver uses), modifiers included. */
static const uint8_t linux_to_hid[256] = {
    0x00, 0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a, 0x2b,
    0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12, 0x13, 0x2f, 0x30, 0x28, 0xe0, 0x04, 0x16,
    0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33, 0x34, 0x35, 0xe1, 0x31, 0x1d, 0x1b, 0x06, 0x19,
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xe5, 0x55, 0xe2, 0x2c, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
    0x3f, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5f, 0x60, 0x61, 0x56, 0x5c, 0x5d, 0x5e, 0x57, 0x59,
    0x5a, 0x5b, 0x62, 0x63, 0x00, 0x00, 0x00, 0x44, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x58, 0xe4, 0x54, 0x00, 0xe6, 0x00, 0x4a, 0x52, 0x4b, 0x50, 0x4f, 0x4d, 0x51, 0x4e, 0x49, 0x00,
    0x00, 0x7f, 0x81, 0x80, 0x66, 0x67, 0x00, 0x48, 0x00, 0x85, 0x00, 0x00, 0x00, 0xe3, 0xe7, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70,
    0x71, 0x72, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

uint16_t hv_hid_to_linux(uint8_t usage) {
    static uint16_t table[256];
    static int made;
    if (!made) { /* the same mapping, the other way: the first Linux code for each usage */
        for (int c = 255; c > 0; c--) if (linux_to_hid[c]) table[linux_to_hid[c]] = (uint16_t)c;
        made = 1;
    }
    return table[usage];
}

void hv_xhci_key_linux(HvXhci *x, uint16_t code, int down) {
    if (code < 256) hv_xhci_key(x, linux_to_hid[code], down);
}

/* ---- the Magic Trackpad 2's reports ---- */

/* Build the current report into out: a 12-byte prefix (report ID 2, the press,
   flags, the timestamp) plus nine bytes a finger. The finger's x and y are 13
   bits, centred on the sensor, with y inverted; byte 3 then holds the finger
   type and the 3-bit state (0 none, 3 near, 4 contact, 7 stop), then the touch
   major and minor axes, size and pressure, and finally the touch ID and angle. */
static int tp_build_report(HvXhci *x, uint8_t *out) {
    int n = 0;
    memset(out, 0, (size_t)(12 + 9 * HV_TP_FINGERS));
    out[0] = 0x02;
    out[1] = (uint8_t)(x->tp_button & 1);
    out[8] = 0x31;
    for (int i = 0; i < HV_TP_FINGERS; i++) {
        const struct HvTpFinger *f = &x->tp_fingers[i];
        if (!f->in) continue;
        int16_t X = (int16_t)(f->x - HV_TP_SENSOR_X / 2);
        int16_t Y = (int16_t)(-(f->y - HV_TP_SENSOR_Y / 2));
        uint8_t off = f->state == 0 || f->state == 7; /* none and stop carry no touch data */
        uint8_t b[9];
        b[0] = (uint8_t)(X & 0xff);
        b[1] = (uint8_t)(((X >> 8) & 0x1f) | ((Y & 0x7) << 5)); /* x's high five bits, then y's low three */
        b[2] = (uint8_t)((Y >> 3) & 0xff);
        b[3] = (uint8_t)(((Y >> 11) & 0x3) | (off ? 0 : (2u << 2)) | (f->state << 5));
        b[4] = off ? 0 : 20;   /* touch major */
        b[5] = off ? 0 : 20;   /* touch minor */
        b[6] = off ? 0 : 10;   /* size */
        b[7] = off ? 0 : (uint8_t)(x->tp_button ? 120 : 5); /* pressure */
        b[8] = (uint8_t)((f->id & 0xf) | (4u << 5)); /* angle: straight */
        memcpy(out + 12 + 9 * n, b, 9);
        n++;
    }
    if (n || x->tp_button) out[7] = 0x03; else out[7] = 0x02; /* touch active */
    out[9] = (uint8_t)((x->tp_ts << 3) | 4);
    out[10] = (uint8_t)((x->tp_ts >> 5) & 0xff);
    out[11] = (uint8_t)((x->tp_ts >> 13) & 0xff);
    return 12 + 9 * n;
}

/* The report timestamp is milliseconds, as the real pad sends it: the guest divides
   position deltas by timestamp deltas to get finger speed. It must be the touch sample's
   own clock, not the moment we process it -- if several samples are handled in one burst,
   wall-clock stamps them all ~1 ms apart and the guest reads a flick as enormous speed,
   which is what sends scroll inertia flying on release. tp_now_ms is only the fallback
   for the physical button, which has no sample clock. */
static uint32_t tp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

/* Queue the current state as a report, stamped with the touch sample's clock (ms);
   ts_ms 0 falls back to the current time (the physical button). */
static void tp_queue(HvXhci *x, uint32_t ts_ms) {
    uint8_t r[HV_TP_REPORT_LEN];
    /* Strictly increasing: two frames with the same timestamp would give the guest a
       zero time delta (speed = distance / 0) and it can throw the frame away. */
    uint32_t now = ts_ms ? ts_ms : tp_now_ms();
    x->tp_ts = now > x->tp_ts ? now : x->tp_ts + 1;
    int len = tp_build_report(x, r);
    if (x->tp_rtail - x->tp_rhead < HV_TP_REPORTS) {
        unsigned at = x->tp_rtail % HV_TP_REPORTS;
        memcpy(x->tp_reports[at], r, (size_t)len);
        x->tp_rlen[at] = (uint8_t)len;
        x->tp_rtail++;
    }
}

static struct HvTpFinger *tp_new_finger(HvXhci *x) {
    for (int i = 0; i < HV_TP_FINGERS; i++) if (!x->tp_fingers[i].in) return &x->tp_fingers[i];
    return NULL;
}

void hv_xhci_touch(HvXhci *x, int id, int phase, int px, int py, uint32_t ts_ms) {
    if (!x || id < 1 || id > 15) return;
    pthread_mutex_lock(&x->mu);
    if (phase == 0) {
        struct HvTpFinger *f = tp_new_finger(x);
        if (f) {
            f->in = 1;
            f->id = id;
            f->x = px;
            f->y = py;
            f->state = 4; /* contact: a touch, not a hover, so the guest tracks it at once */
            tp_queue(x, ts_ms);
        }
    } else {
        for (int i = 0; i < HV_TP_FINGERS; i++) {
            struct HvTpFinger *f = &x->tp_fingers[i];
            if (!f->in || f->id != id) continue;
            if (phase == 1) {
                f->x = px;
                f->y = py;
                if (f->state == 3) f->state = 4;
                /* no report yet: the caller commits one frame holding every finger that
                   moved, so the guest sees them move together instead of one at a time */
            } else {
                f->state = 7; /* stop */
                tp_queue(x, ts_ms);
                f->in = 0;
                f->state = 0; /* none */
                tp_queue(x, ts_ms);
            }
            break;
        }
    }
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

/* Commit one frame with the current state of all the fingers: a move batch ends
   here, so a gesture's fingers all land in the same report. */
void hv_xhci_touch_frame(HvXhci *x, uint32_t ts_ms) {
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    tp_queue(x, ts_ms);
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

void hv_xhci_touch_button(HvXhci *x, int down) {
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    if (x->tp_button != (down & 1)) {
        x->tp_button = down & 1;
        tp_queue(x, 0);
    }
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

void hv_xhci_pointer(HvXhci *x, int px, int py, int buttons, int wheel) {
    if (!x || !x->pc) return;
    px = px < 0 ? 0 : px > 32767 ? 32767 : px;
    py = py < 0 ? 0 : py > 32767 ? 32767 : py;
    uint8_t r[6] = {(uint8_t)(buttons & 7), (uint8_t)px, (uint8_t)(px >> 8), (uint8_t)py, (uint8_t)(py >> 8), (uint8_t)(int8_t)wheel};
    pthread_mutex_lock(&x->mu);
    /* A move on top of a move that has not gone out yet replaces it: the guest wants where the pointer is. */
    uint8_t *last = x->tab_rtail != x->tab_rhead ? x->tab_reports[(x->tab_rtail - 1) % HV_TAB_REPORTS] : NULL;
    if (last && last[0] == r[0] && !last[5] && !r[5]) memcpy(last, r, 6);
    else if (x->tab_rtail - x->tab_rhead < HV_TAB_REPORTS) memcpy(x->tab_reports[x->tab_rtail++ % HV_TAB_REPORTS], r, 6);
    memcpy(x->tab_state, r, 5);
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

void hv_xhci_poll(HvXhci *x) {
    if (!x || !(x->usbcmd & 1)) return; /* stopped: the rings it knew may be anyone's memory by now */
    for (int slot = 1; slot <= HV_XHCI_SLOTS; slot++) {
        if (x->slots[slot].dev < 0) continue;
        if (x->pc) pc_run(x, slot, 3);
        else run_ep_in(x, slot);
    }
}

static HvXhci *create(HvMachine *m, HvPcie *bus, int pc, HvDisk *cdrom) {
    HvXhci *x = calloc(1, sizeof *x);
    x->m = m;
    x->pc = pc;
    x->cd = cdrom;
    x->kind[0] = HV_USB_KEYBOARD;
    x->kind[1] = pc ? HV_USB_TABLET : HV_USB_TRACKPAD;
    x->kind[2] = HV_USB_CDROM;
    x->nports = pc && cdrom ? 3 : 2;
    pthread_mutex_init(&x->mu, NULL);
    /* QEMU's xHCI IDs. Class 0x0c0330 with revision 0: macOS's IOPCIClassMatch compares all 32 bits. */
    hv_pci_init(&x->pci, 0x1b36, 0x000d, 0x0c033000, 0x1af4, 0x1100);
    x->pci.opaque = x;
    x->pci.bar_read = bar_read;
    x->pci.bar_write = bar_write;
    hv_pci_add_bar(&x->pci, 0, BAR_SIZE, 1);
    x->pci.config[0x60] = 0x30; /* SBRN: USB 3.0 controller */
    x->pci.config[0x61] = 0x20; /* FLADJ default */
    if (hv_pcie_attach(bus, &x->pci) < 0) {
        free(x);
        return NULL;
    }
    reset(x);
    return x;
}

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus) { return create(m, bus, 0, NULL); }
HvXhci *hv_xhci_create_pc(HvMachine *m, HvPcie *bus, HvDisk *cdrom) { return create(m, bus, 1, cdrom); }
