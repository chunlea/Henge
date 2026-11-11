#ifndef HENGEVM_XHCI_H
#define HENGEVM_XHCI_H

#include <pthread.h>

#include "disk.h"
#include "pcie.h"

/* An xHCI USB host controller (xHCI 1.0, one interrupter, 32-byte contexts,
   INTx) with two USB HID devices: a boot keyboard on port 1 and a Magic
   Trackpad 2 on port 2: what Virtualization.framework gives a macOS guest as
   its "Virtual USB Keyboard", plus Apple's multitouch pad, so the guest's
   own gesture engine sees a real trackpad. Changes come from the host UI on
   any thread (hv_xhci_key, hv_xhci_touch); the CPU thread delivers them
   (hv_xhci_poll). */
#define HV_XHCI_SLOTS 8
#define HV_XHCI_PORTS 2      /* the virtual Mac's */
#define HV_XHCI_MAX_PORTS 6
#define HV_XHCI_DEVICES 2
/* What can sit on a port. The virtual Mac has the first two. The UEFI board (hv_xhci_create_pc) has a
   keyboard, a tablet (a pointer that reports where it is on the screen, as QEMU's usb-tablet does: no
   mouse to capture) and, with installation media, a USB CD-ROM drive (mass storage, bulk-only, SCSI):
   devices Windows has drivers for out of the box. So is the network card, a CDC NCM function. */
enum { HV_USB_KEYBOARD, HV_USB_TRACKPAD, HV_USB_TABLET, HV_USB_CDROM, HV_USB_NET, HV_USB_SCREEN };
#define HV_SCREEN_FINGERS 5
#define HV_SCREEN_REPORT (1 + HV_SCREEN_FINGERS * 6 + 3) /* the ID; tip, contact, X, Y for each finger; how many; the scan time */
#define HV_TAB_REPORTS 64
#define HV_XHCI_INTRS 4
#define HV_XHCI_EPS 32 /* device context index 1..31 */
#define HV_XHCI_REPORTS 256

/* The Magic Trackpad 2's sensor, in its own units (a ~160 x 115 mm pad).
   hv_xhci_touch takes coordinates in these ranges. */
#define HV_TP_SENSOR_X 8134
#define HV_TP_SENSOR_Y 5206
#define HV_TP_FINGERS 10
#define HV_TP_REPORTS 64
#define HV_TP_REPORT_LEN (12 + 9 * HV_TP_FINGERS) /* report ID 2: 12-byte prefix, N x 9-byte fingers */

typedef struct HvXhciRing {
    uint64_t dequeue; /* guest PA of the next TRB */
    int ccs;          /* consumer cycle state */
} HvXhciRing;

typedef struct HvXhci {
    HvPciDev pci;
    HvMachine *m;
    /* operational */
    uint32_t usbcmd, usbsts, dnctrl, config;
    uint64_t dcbaap;
    HvXhciRing cmd;
    int cmd_running;
    uint32_t portsc[HV_XHCI_MAX_PORTS];
    int nports;
    int kind[HV_XHCI_MAX_PORTS]; /* HV_USB_*: the device on each port */
    int pc;                      /* the UEFI board's set of devices */
    /* interrupters and their event rings (macOS's driver wants at least three) */
    struct HvXhciIntr {
        uint32_t iman, imod, erstsz;
        uint64_t erstba, erdp;
        uint64_t seg_base;  /* current event ring segment */
        uint32_t seg_size, seg_index, index;
        int pcs;
    } intr[HV_XHCI_INTRS];
    /* device slots: -1 until the device on the slot's port is addressed */
    int slot_used[HV_XHCI_SLOTS + 1];
    struct HvXhciSlot {
        int dev;         /* 0 keyboard, 1 trackpad, -1 none */
        int configured;
        HvXhciRing ep[HV_XHCI_EPS];
        int ep_running[HV_XHCI_EPS];
        /* pc: the control transfer in progress */
        uint8_t setup[8];
        uint8_t reply[512];
        int reply_len;   /* -1: the request is refused (a stall at its next stage) */
    } slots[HV_XHCI_SLOTS + 1];
    /* keyboard reports waiting for an IN transfer (8 bytes each) */
    pthread_mutex_t mu;
    uint8_t reports[HV_XHCI_REPORTS][8];
    uint32_t rhead, rtail;
    uint8_t state[8];      /* modifiers, reserved, six keys: the current report */
    uint64_t delivered;
    /* Magic Trackpad 2: the fingers on the pad and their reports (Apple
       multitouch format, report ID 2: 12-byte prefix plus 9 bytes a finger). */
    struct HvTpFinger {
        int in;
        int id;        /* the report's 4-bit touch ID, 1..15 */
        int x, y;      /* sensor coordinates, 0..HV_TP_SENSOR_X/Y */
        uint8_t state; /* the report's 3-bit state: 0 none, 3 near, 4 contact, 7 stop */
    } tp_fingers[HV_TP_FINGERS];
    int tp_button;     /* the pad's full press (report byte 1, bit 0) */
    uint32_t tp_ts;    /* the report timestamp, one per report */
    uint8_t tp_reports[HV_TP_REPORTS][HV_TP_REPORT_LEN];
    uint8_t tp_rlen[HV_TP_REPORTS];
    uint32_t tp_rhead, tp_rtail;
    int tp_mt;         /* the guest switched us to multitouch mode (feature report 2) */
    uint64_t tp_delivered;
    uint32_t tp_reqs;   /* control requests logged, capped */
    uint32_t tp_db;     /* doorbells logged, capped */
    uint32_t tp_multi_logs; /* multi-finger deliveries logged, capped */
    uint8_t tp_f1[8];   /* feature report 1: the header for the selected sensor register */
    int tp_f1_len;
    /* The tablet: reports waiting for an IN transfer (buttons, x, y, wheel: 6 bytes each), under mu. */
    uint8_t tab_reports[HV_TAB_REPORTS][6];
    uint32_t tab_rhead, tab_rtail;
    uint8_t tab_state[6];
    /* The touch screen: its fingers as last told (hv_xhci_screen_touch), and the reports made of them at each
       frame, waiting for an IN transfer; under mu. */
    struct { uint8_t down, lifted; uint16_t x, y; } scr_finger[HV_SCREEN_FINGERS];
    uint8_t scr_reports[HV_TAB_REPORTS][HV_SCREEN_REPORT];
    uint32_t scr_rhead, scr_rtail;
    /* The CD-ROM drive: one command at a time, as bulk-only transport has it. */
    HvDisk *cd;
    int msc_state;         /* waiting for a command, sending data, taking data, or owing the status */
    uint32_t msc_tag;
    uint32_t msc_expected; /* bytes the host said the command would move */
    uint32_t msc_moved;
    uint8_t msc_failed;
    uint8_t sense_key, sense_asc;
    uint8_t *msc_buf;      /* the data on its way to the host */
    uint64_t msc_cap;
    uint64_t msc_len, msc_off;
    /* The network card (CDC NCM). The host side is a pair of hooks, as virtio-net's is: transmit gets
       every frame the guest sends, and frames for the guest go in through hv_xhci_net_receive. */
    uint8_t net_mac[6];
    int net_alt;           /* the data interface's alternate setting: 1 once the guest brings the network up */
    int net_notify;        /* notifications owed: 2 the link's speed and then its state, 1 its state */
    uint16_t net_seq;
    uint8_t *net_buf;      /* one NTB */
    void (*net_transmit)(void *opaque, const uint8_t *frame, uint32_t len);
    void *net_opaque;
    uint64_t net_tx_frames, net_rx_frames, net_rx_dropped;
} HvXhci;

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus);
/* The UEFI board's: a keyboard, a tablet and, if cdrom is not NULL, a CD-ROM drive holding it, and if
   mac is not NULL, a network card with that address. */
HvXhci *hv_xhci_create_pc(HvMachine *m, HvPcie *bus, HvDisk *cdrom, const uint8_t *mac);
/* CPU thread: a frame for the guest. 0 if the guest has no transfer waiting for one (the caller keeps
   the frame for later). */
int hv_xhci_net_receive(HvXhci *x, const uint8_t *frame, uint32_t len);
/* Any thread: where the tablet's pointer is (0..32767 across the display, each way) and which buttons
   are down (bit 0 left, 1 right, 2 middle); wheel is a number of notches turned since the last call. */
void hv_xhci_pointer(HvXhci *x, int px, int py, int buttons, int wheel);
/* Any thread: the UEFI board's touch screen. A finger (0 to 4) comes down or moves (down 1) or lifts (0) at
   px, py, 0..32767 across the display each way; hv_xhci_screen_frame then reports the fingers as they are,
   all in one report, as a touch screen's scan does. */
void hv_xhci_screen_touch(HvXhci *x, int finger, int down, int px, int py);
void hv_xhci_screen_frame(HvXhci *x, uint32_t ts_ms);
/* Any thread: a key went down or up (HID usage page 7 usage, e.g. 4 = A; 0xe0-0xe7 are modifiers). */
void hv_xhci_key(HvXhci *x, uint8_t usage, int down);
/* The same, by Linux input key code (what the host UI produces). */
void hv_xhci_key_linux(HvXhci *x, uint16_t code, int down);
/* Any thread: a Magic Trackpad 2 touch. phase: 0 down, 1 move, 2 up. id: a
   stable 1..15 for this touch; x, y in sensor units (HV_TP_SENSOR_*). Down and up
   report at once; a move only updates the finger, and hv_xhci_touch_frame commits
   one report for all the fingers that moved together. ts_ms is the touch sample's
   own clock (ms); the guest divides position deltas by timestamp deltas to get
   speed, so it must be the sample time, not the moment we happen to process it. */
void hv_xhci_touch(HvXhci *x, int id, int phase, int px, int py, uint32_t ts_ms);
/* Any thread: send one frame with every finger's current state (ends a move batch),
   stamped with the touch sample's own clock (ms). */
void hv_xhci_touch_frame(HvXhci *x, uint32_t ts_ms);
/* Any thread: the pad's full press (a click), down or up. */
void hv_xhci_touch_button(HvXhci *x, int down);
/* CPU thread: hand waiting reports to the guest. */
void hv_xhci_poll(HvXhci *x);

/* A HID keyboard usage as a Linux key code (0 if none). */
uint16_t hv_hid_to_linux(uint8_t usage);

#endif
