// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_VIRTIO_GPU_H
#define HENGEVM_VIRTIO_GPU_H

#include <stdint.h>

#include "virtio.h"

/* virtio-gpu, 2D (virtio spec 5.7, no virgl): a KMS device for a Linux guest,
   which Linux's virtio_gpu driver turns into a DRM card with one CRTC, a
   primary and a cursor plane. The guest renders with its own CPUs (Mesa's
   software rasteriser) into resources it backs with its RAM; a transfer copies
   them to host memory and a flush of the scanout's resource puts it on the
   display, which is a host buffer the app reads (hv_vm_boot_display). */
typedef struct HvGpuResource HvGpuResource;
typedef struct HvGpuVirgl HvGpuVirgl;

typedef struct HvVirtioGpu {
    HvVirtio vio;
    HvMachine *m;
    HvGpuResource *res;
    uint32_t width, height;    /* the one scanout's mode */
    uint32_t *display;         /* width * height, B8G8R8X8 (host memory) */
    uint32_t scanout_res;      /* the resource on the scanout, 0: none */
    uint64_t flushes;
    uint64_t changes;          /* bumped by each flush and cursor command: the picture may differ */
    /* The cursor (the cursor queue): a 64x64 image, where it is, and its hot spot. */
    uint32_t cursor[64 * 64];
    int cursor_on;
    uint64_t cursor_gen;       /* bumped when the cursor's image (or whether there is one) changes */
    int32_t cursor_x, cursor_y, hot_x, hot_y;
    HvGpuVirgl *virgl;         /* 3D (virglrenderer), or NULL: 2D only */
    /* A device whose guest has a Direct3D driver for it (hv_virtio_gpu_create_d3d). */
    int d3d;
    long long ask_until, ask_next; /* display events are repeated until the picture has the size wanted (virtio_gpu.c, ask_size): 0 not begun, -1 over */
    int enabled;               /* a resource is on the scanout: the picture is this device's (not the firmware's framebuffer) */
    uint32_t want_w, want_h;   /* the size the display would like to be (EDID, display info) */
    uint64_t hostmem_size;     /* the host-visible shared memory region: BAR 2, this big */
    uint64_t hostmem_pa;       /* where the guest has it, once something was mapped (0: not yet) */
    HvRam *hostmem_ram;        /* and the machine's region there: its pages are the mapped blobs' */
} HvVirtioGpu;

/* display: where the picture goes, width * height 32-bit pixels. */
HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, uint32_t width, uint32_t height, uint32_t *display);
/* A device for a guest with UTM's Direct3D driver (Windows, viogpu3d): virglrenderer with its Neptune protocol,
   blob resources and memory the host maps into the guest. The guest chooses the picture's size (up to
   GPU_MAX_W x GPU_MAX_H; width x height is what it is offered first), so the device has its own display buffer.
   dir: the folder of frameworks (virglrenderer, ANGLE, dxmt-native). NULL if 3D cannot start. */
HvVirtioGpu *hv_virtio_gpu_create_d3d(HvMachine *m, HvPcie *bus, uint32_t width, uint32_t height, const char *dir);
/* Device lock held: the host would like a Direct3D device's display to be this size. The driver is told its
   display changed and asks again what it is like (display info, EDID); UTM's driver then has Windows change
   to that size, when its helper service (vgpusrv) runs in the guest. */
void hv_virtio_gpu_want_size(HvVirtioGpu *g, uint32_t width, uint32_t height);
/* The display with the cursor drawn on it, into out (width * height). */
void hv_virtio_gpu_compose(HvVirtioGpu *g, uint32_t *out);
/* CPU 0, device lock held: 3D requests that have finished go back to the guest. */
void hv_virtio_gpu_poll(HvVirtioGpu *g);
/* With the CPUs stopped: the device, its resources and its 3D side. */
void hv_virtio_gpu_destroy(HvVirtioGpu *g);

#endif
