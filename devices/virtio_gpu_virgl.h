// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_VIRTIO_GPU_VIRGL_H
#define HENGEVM_VIRTIO_GPU_VIRGL_H

#include "virtio_gpu.h"

/* virtio-gpu's 3D side: virglrenderer on ANGLE (OpenGL ES on Metal), loaded from the frameworks in dir.
   NULL when they cannot be loaded or started (the device then stays 2D). */
HvGpuVirgl *hv_gpu_virgl_start(HvVirtioGpu *g, const char *dir);
/* Route the device's queues to it (once the virtqueues exist). */
void hv_gpu_virgl_attach(HvGpuVirgl *v);
/* CPU 0, device lock held: finished requests back to the guest. */
void hv_gpu_virgl_poll(HvGpuVirgl *v);
/* With the CPUs stopped: ends the worker after it releases its resources, contexts and virglrenderer. */
void hv_gpu_virgl_stop(HvGpuVirgl *v);

#endif
