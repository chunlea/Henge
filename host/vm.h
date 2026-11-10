#ifndef HENGEVM_VM_H
#define HENGEVM_VM_H

#include <stddef.h>
#include <stdint.h>

#include "disk.h"
#include "vmapple.h"

/* The virtual Mac as a host app drives it: build it from files, run it, feed
   it input, stop it. The macOS command-line tool and the iOS app both use this;
   graphics are the host's own business (Mac: hv_pvg_attach before starting). */
typedef struct HvVmConfig {
    const char *rom;          /* AVPBooter.vmapple2.bin */
    const char *disk, *aux;   /* the root disk image and the auxiliary storage */
    uint64_t aux_offset;      /* where the aux image's data starts (16 KB) */
    uint64_t ecid, seed;      /* the machine identity the disk was installed with */
    uint64_t ram_mb;
    int ncpus;
    int log;                  /* m->log */
    int no_net, no_sound;
    int installer;            /* boot the ROM in DFU mode with the USB device port (installing macOS) */
    /* JIT memory the host already has (iOS: granted by a debugger), or NULL to map it here. */
    void *jit_rx, *jit_rw;
    size_t jit_bytes;         /* 0: 768 MB */
    /* A Linux guest instead of macOS (host/linux_boot.h): no ROM or aux then. */
    const char *linux_kernel, *linux_initrd, *linux_cmdline;
    int linux_from_disk;      /* Linux, booting what the disk's bootloader would (kernel NULL) */
    const char *linux_extra;  /* appended to the kernel command line */
    uint32_t fb_width, fb_height;  /* its display; 0: 1280x800 */
} HvVmConfig;

/* Returns 0, or -1 with a reason in err. */
int hv_vm_create(const HvVmConfig *cfg, char *err, size_t errlen);
/* After CPUs stop, detach backends and release the old machine. Borrowed iOS
   JIT memory stays owned by the app and can be attached to the next machine. */
void hv_vm_destroy(void);
HvMachine *hv_vm_machine(void);
HvVmapple *hv_vm_board(void);
HvCpu *hv_vm_cpu(int index);
extern void (*hv_prof_register)(void); /* debugging: each thread that will run guest code calls this */
/* Frames go to this CAMetalLayer, drawn by the GPU, instead of hv_vm_boot_display. */
void hv_vm_set_display_layer(void *layer);
uint64_t hv_vm_gpu_frames(void);
int hv_vm_ncpus(void);
int hv_vm_is_linux(void);
/* The serial port (PL011): what the guest writes goes to out (on a CPU thread; NULL: stdout),
   and hv_vm_serial_input queues bytes for it to read. Returns how many fit. */
void hv_vm_set_serial_output(void (*out)(void *opaque, uint8_t c), void *opaque);
size_t hv_vm_serial_input(const uint8_t *buf, size_t len);

/* The boot framebuffer iBoot hands the kernel (XNU's boot_args video info):
   the Apple logo and progress bar while booting or installing, before a GPU
   driver takes the display. Host pointer to the pixels (32-bit BGRA), or NULL
   until the kernel has its boot_args. */
typedef struct HvBootDisplay {
    const uint8_t *pixels;
    uint32_t width, height, stride;
} HvBootDisplay;
int hv_vm_boot_display(HvBootDisplay *d);
/* Linux with virtio-gpu: a count that moves whenever hv_vm_boot_display's picture may have changed,
   so a viewer can skip unchanged frames. 0: no such count (look every time). */
uint64_t hv_vm_display_changes(void);

/* Linux drawing with the GPU and HV_VIRGL_PRESENT set: the IOSurface holding its newest frame, retained (the caller
   releases it), to put in a layer; NULL when the picture is only hv_vm_boot_display's. */
struct __IOSurface *hv_vm_linux_surface(void);
/* Linux's pointer, which is not in that surface: where it is, its hot spot, and its 64 x 64 BGRA image (alpha in
   the top byte), which changes when gen does. 0, or -1 when there is no such display. */
typedef struct HvLinuxCursor {
    int on;
    int32_t x, y, hot_x, hot_y;
    uint64_t gen;
    const uint32_t *pixels;
} HvLinuxCursor;
int hv_vm_linux_cursor(HvLinuxCursor *c);

/* Extra device work on CPU 0 each tick (Mac graphics). Set before hv_vm_start. */
void hv_vm_set_poll(void (*poll)(void));

/* Runs CPU 0 on a new thread (unless the caller runs it: cpu0 = 0), the other
   CPUs and the 1 ms kicker. on_exit is called on CPU 0's thread when the
   machine stops (powered off, a fatal error, or hv_vm_stop), after the disks
   are flushed; reason is an HV_EXIT_* value. */
/* Returns -1 if a thread could not start; already-started workers are joined. */
int hv_vm_start(int cpu0, void (*on_exit)(int reason));
/* Call on a thread before it runs guest code (hv_vm_start's threads do): see vm.c. */
void hv_vm_prepare_cpu_thread(void);
/* That, and the scheduling class the guest's CPUs run at. */
void hv_vm_cpu_qos(void);
/* Run CPU 0 on the calling thread (what hv_vm_start does on its own thread); returns its exit reason. */
int hv_vm_run_cpu0(void);
/* Stop at the next block; the CPUs' threads end. */
void hv_vm_stop(void);
void hv_vm_flush_disks(void);
const char *hv_vm_reason_name(int reason);

/* Input from the host UI, any thread. Keys are Linux input codes (KEY_A = 30). */
void hv_vm_key(uint16_t code, int down);
void hv_vm_key_hid(uint8_t usage, int down); /* HID keyboard usage, which is what iOS reports for a hardware keyboard */
void hv_vm_mouse_move(int dx, int dy);
void hv_vm_mouse_button(int button, int down); /* 0 left, 1 right, 2 middle */
void hv_vm_mouse_wheel(int dy);
void hv_vm_mouse_hwheel(int dx);
/* The Magic Trackpad 2 on the USB bus: a finger on it. phase: 0 down, 1 move, 2 up; id a
   stable 1..15 per touch; x, y in sensor units (HV_TP_SENSOR_* in devices/xhci.h).
   ts_ms is the touch sample's own clock in ms (see hv_xhci_touch). */
void hv_vm_touch(int id, int phase, int x, int y, uint32_t ts_ms);
void hv_vm_touch_frame(uint32_t ts_ms); /* commit one frame with every finger that moved */
void hv_vm_touch_button(int down); /* the pad's full press (a click) */
/* Where the guest has its pointer, in display pixels. 0, or -1 before it has one. */
int hv_vm_pointer(int *x, int *y);
/* A small picture of the display, BGRA, width * height * 4 bytes. 0, or -1 before anything shows. */
int hv_vm_snapshot(uint8_t *bgra, int width, int height);

#endif
