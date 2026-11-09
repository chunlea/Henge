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

typedef struct HvVirtioGpu {
    HvVirtio vio;
    HvMachine *m;
    HvGpuResource *res;
    uint32_t width, height;    /* the one scanout's mode */
    uint32_t *display;         /* width * height, B8G8R8X8 (host memory) */
    uint32_t scanout_res;      /* the resource on the scanout, 0: none */
    uint64_t flushes;
    /* The cursor (the cursor queue): a 64x64 image, where it is, and its hot spot. */
    uint32_t cursor[64 * 64];
    int cursor_on;
    int32_t cursor_x, cursor_y, hot_x, hot_y;
} HvVirtioGpu;

/* display: where the picture goes, width * height 32-bit pixels. */
HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, uint32_t width, uint32_t height, uint32_t *display);
/* The display with the cursor drawn on it, into out (width * height). */
void hv_virtio_gpu_compose(HvVirtioGpu *g, uint32_t *out);

#endif
