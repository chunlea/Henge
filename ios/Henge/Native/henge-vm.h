/* The engine (HengeVM, ../host/vm.h) as the Swift side sees it. */
#ifndef HENGE_VM_H
#define HENGE_VM_H

#include <stddef.h>
#include <stdint.h>

/* Builds the machine from imported files, on the JIT region the app holds
   (henge_ios_jit_prewarm), and starts it. 0, or -1 with a reason in err. */
int henge_vm_boot(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                  int cpus, char *err, size_t errlen);
/* A Linux machine: boots what the disk's own boot entry names (Limine's limine.conf on its EFI partition,
   host/linux_disk.h), with a width x height framebuffer as its display. extra is added to the kernel's
   command line (may be NULL). 0, or -1 with a reason in err. */
int henge_vm_boot_linux(const char *disk, uint64_t ram_mb, int cpus, uint32_t width, uint32_t height, const char *extra,
                        char *err, size_t errlen);
/* A machine that starts from UEFI firmware (Windows on Arm; host/uefi_boot.h): the firmware (EDK2's ArmVirtQemu),
   the file its variables are kept in, the disk, and installation media (an ISO; may be NULL) in a USB CD-ROM
   drive. width x height is the display the firmware offers (0: its own 800 x 600), which is the largest
   Windows can then be set to. gpu: the folder of GPU frameworks, for a GPU Windows uses once UTM's driver
   (viogpu3d) is installed in it, or NULL for none. 0, or -1 with a reason in err. */
int henge_vm_boot_uefi(const char *firmware, const char *vars, const char *disk, const char *cdrom, uint64_t ram_mb,
                       int cpus, uint32_t width, uint32_t height, const char *gpu, char *err, size_t errlen);
/* Unpacks a gzip file (the firmware as the app carries it). 0 or -1. */
int henge_gunzip(const char *from, const char *to);
/* Unpacks a Zstandard disk image published in parts (host/linux_image.h) into a sparse disk, deleting each part
   once read. Progress: henge_linux_unpack_progress (0..1). 0, or -1 with a reason in err. Blocking. */
int henge_linux_unpack(const char *const *parts, int nparts, const char *disk, char *err, size_t errlen);
double henge_linux_unpack_progress(void);
/* Whether the disk has a boot entry Henge can start (a Linux disk). */
int henge_linux_disk_bootable(const char *disk);
/* Writes to the running Linux machine's serial console. */
void henge_vm_serial_write(const char *text);

/* Installs macOS from an IPSW onto a new machine (blank disk and auxiliary
   storage): boots the ROM in DFU mode and runs the restore over its USB port,
   as Virtualization.framework's installer does. workdir is scratch space.
   0, or -1 with a reason in err; progress below. */
int henge_vm_install(const char *rom, const char *disk, const char *aux, uint64_t ecid, const char *ipsw,
                     const char *workdir, uint64_t ram_mb, int cpus, char *err, size_t errlen);
/* The restore's step (0 detect, 1 prepare, 2 system image, 3 verify, 4 firmware,
   6 FUD, 7 personalized images; 100 done, 101 failed) and how far it is (0..1). */
int henge_install_step(void);
double henge_install_progress(void);
/* Writes a blank auxiliary storage file (what VZMacAuxiliaryStorage makes). 0 or -1. */
int henge_make_aux(const char *path);

/* 0 not started, 1 running, 2 stopped, 3 starting/stopping.
   Normal guests are fully released before state 2; installer sessions retain
   their restore stack and can_start stays false. */
int henge_vm_state(void);
/* A new generation starts with each successful boot, even in the same view. */
uint64_t henge_vm_generation(void);
int henge_vm_can_start(void);
/* Human-readable terminal reason, once state is 2. */
const char *henge_vm_exit_reason(void);
/* Terminal outcome after state == 2. Generic HALT alone does not prove a
   guest shutdown (it also covers reset, host stop and guest panic). */
enum HengeVmOutcome {
    HENGE_VM_OUTCOME_ERROR, HENGE_VM_OUTCOME_SHUTDOWN, HENGE_VM_OUTCOME_RESTART,
    HENGE_VM_OUTCOME_HOST_STOP, HENGE_VM_OUTCOME_PANIC
};
int henge_vm_exit_outcome(void);
void henge_vm_stop(void);
/* Testing: sets a translation knob (core/machine.h) in the running machine; nonzero if it exists. */
int henge_vm_knob(const char *name, int value);
/* The memory balloon (only with HV_BALLOON set before boot): asks the guest to hand over target_mb (when >= 0)
   and returns how many MB it has handed over, or -1 without a balloon. */
long henge_vm_balloon(long target_mb);

/* Progress, for the session screen. */
uint64_t henge_vm_blocks(void);            /* blocks the CPUs entered */
uint64_t henge_vm_net_frames(int sent);    /* Ethernet frames out (sent = 1) and in */
uint64_t henge_vm_dns(int which);          /* guest DNS queries answered through iOS's resolver (0), handed to libslirp (1), unanswered in time (2) */
int henge_vm_cpus_on(void);                /* CPUs the guest has started */
int henge_vm_guest_el0(void);              /* any CPU running user code right now */

/* Input: Linux key codes, relative pointer, buttons 0 left 1 right 2 middle. */
/* The shared folder and clipboard (host/share.c). henge_vm_share goes before henge_vm_boot; agent may be NULL.
   henge_share_take_clipboard returns what the guest copied, once: a malloc'd string, or NULL. */
void henge_vm_share(const char *folder, const char *socket_path, const char *agent);
void henge_share_set_clipboard(const char *text);
char *henge_share_take_clipboard(void);
long henge_share_agent_seen(void);

/* The size macOS is told its screen is. Before henge_vm_boot. */
void henge_vm_set_display(uint32_t width, uint32_t height);
/* The panel's physical size; 0, 0 for the default (a ~122 ppi, 1x screen). Before boot. */
void henge_vm_set_display_mm(uint32_t width_mm, uint32_t height_mm);
void henge_vm_key(uint16_t code, int down);
void henge_vm_key_hid(uint8_t usage, int down); /* a hardware keyboard: iOS gives each key's HID usage */
void henge_vm_mouse_move(int dx, int dy);
void henge_vm_mouse_button(int button, int down);
/* Wheel units: dy > 0 up, dx > 0 right. */
void henge_vm_mouse_scroll(int dx, int dy);
/* The Magic Trackpad 2 the Mac sees on its USB bus: a finger on it. phase: 0 down, 1 move, 2 up;
   finger a stable 1..15 per touch; x, y sensor units 0..8134, 0..5206. ts_ms is the touch
   sample's own clock in ms, so the guest computes finger speed from real sample timing. */
void henge_vm_touch(int finger, int phase, int x, int y, uint32_t ts_ms);
void henge_vm_touch_frame(uint32_t ts_ms); /* commit one frame with every finger that moved */
void henge_vm_touch_button(int down); /* the pad's full press (a click) */
/* Where the guest has its pointer, in display pixels. 0, or -1 before it has one. */
int henge_vm_pointer(int *x, int *y);
/* A small picture of the display, BGRA, width * height * 4 bytes. 0, or -1 before anything shows. */
int henge_vm_snapshot(uint8_t *bgra, int width, int height);

/* The boot framebuffer (Apple logo and progress bar), 32-bit BGRA, or -1 if
   the kernel has not got that far. A successful call returns an owned copy;
   release it with henge_vm_free_display after use. */
/// Where the Mac's GPU frames go: a CAMetalLayer. Keep it alive while the machine runs.
void henge_vm_set_display_layer(void *layer);
/// How many frames have gone to it.
uint64_t henge_vm_gpu_frames(void);
int henge_vm_boot_display(const uint8_t **pixels, int *width, int *height, int *stride);
/// Linux: moves whenever the picture henge_vm_boot_display gives may have changed; 0 when there is no such count.
uint64_t henge_vm_display_changes(void);
void henge_vm_free_display(const uint8_t *pixels);
/// Linux drawing with the GPU: the IOSurface (an IOSurfaceRef, retained: release it) holding its newest frame, to set
/// as a layer's contents. NULL when there is none, and henge_vm_boot_display is the picture.
void *henge_vm_linux_surface(void);
/// Linux's pointer, drawn by the app over that surface: position and hot spot in the screen's pixels. gen moves when
/// its image does; pixels (64 x 64 BGRA, premultiplied or not as the guest gave it), if not NULL, gets that image.
/// Returns 1 when the pointer is to be shown.
int henge_vm_linux_cursor(int32_t *x, int32_t *y, int32_t *hot_x, int32_t *hot_y, uint64_t *gen, uint32_t *pixels);

/* The largest single mapping the address space takes right now, in MB (the
   increased virtual addressing entitlement raises it). */
uint64_t henge_largest_reservation_gb(void); /* measuring: the biggest PROT_NONE reservation, in GB */
uint64_t henge_largest_mapping_mb(void);
/* Bytes this process may still allocate before jetsam (os_proc_available_memory). */
size_t henge_available_memory(void);

#endif
