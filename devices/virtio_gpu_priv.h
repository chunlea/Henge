// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_VIRTIO_GPU_PRIV_H
#define HENGEVM_VIRTIO_GPU_PRIV_H

/* What virtio_gpu.c (2D) and virtio_gpu_virgl.c (3D, virglrenderer) share. */
#include <stdint.h>

#define CONTROLQ 0
#define CURSORQ 1

/* Commands and responses (include/uapi/linux/virtio_gpu.h). */
enum {
    CMD_GET_DISPLAY_INFO = 0x0100,
    CMD_RESOURCE_CREATE_2D,
    CMD_RESOURCE_UNREF,
    CMD_SET_SCANOUT,
    CMD_RESOURCE_FLUSH,
    CMD_TRANSFER_TO_HOST_2D,
    CMD_RESOURCE_ATTACH_BACKING,
    CMD_RESOURCE_DETACH_BACKING,
    CMD_GET_CAPSET_INFO,
    CMD_GET_CAPSET,
    CMD_GET_EDID,
    CMD_RESOURCE_ASSIGN_UUID,
    CMD_RESOURCE_CREATE_BLOB,
    CMD_SET_SCANOUT_BLOB,
    CMD_CTX_CREATE = 0x0200,
    CMD_CTX_DESTROY,
    CMD_CTX_ATTACH_RESOURCE,
    CMD_CTX_DETACH_RESOURCE,
    CMD_RESOURCE_CREATE_3D,
    CMD_TRANSFER_TO_HOST_3D,
    CMD_TRANSFER_FROM_HOST_3D,
    CMD_SUBMIT_3D,
    CMD_RESOURCE_MAP_BLOB,
    CMD_RESOURCE_UNMAP_BLOB,
    CMD_UPDATE_CURSOR = 0x0300,
    CMD_MOVE_CURSOR,
    RESP_OK_NODATA = 0x1100,
    RESP_OK_DISPLAY_INFO,
    RESP_OK_CAPSET_INFO,
    RESP_OK_CAPSET,
    RESP_OK_EDID,
    RESP_OK_RESOURCE_UUID,
    RESP_OK_MAP_INFO,
    RESP_ERR_UNSPEC = 0x1200,
    RESP_ERR_OUT_OF_MEMORY,
    RESP_ERR_INVALID_SCANOUT_ID,
    RESP_ERR_INVALID_RESOURCE_ID,
    RESP_ERR_INVALID_CONTEXT_ID,
    RESP_ERR_INVALID_PARAMETER,
};
#define FLAG_FENCE 1u
#define FLAG_RING_IDX 2u /* the fence is on one of the context's own timelines (ring_idx) */
/* Feature bits. */
#define GPU_F_VIRGL 1ull
#define GPU_F_EDID 2ull
#define GPU_F_RESOURCE_BLOB 8ull
#define GPU_F_CONTEXT_INIT 16ull
#define CAPSET_NEPTUNE 7 /* UTM's Direct3D-over-virtio protocol (virglrenderer's src/neptune) */
/* The largest picture a scanout may be (the display buffer of a device whose guest chooses the size). */
#define GPU_MAX_W 3840u
#define GPU_MAX_H 2400u
#define MAX_SCANOUTS 16

/* Formats. */
enum { B8G8R8A8 = 1, B8G8R8X8 = 2, A8R8G8B8 = 3, X8R8G8B8 = 4, R8G8B8A8 = 67, X8B8G8R8 = 68, A8B8G8R8 = 121, R8G8B8X8 = 134 };

typedef struct Hdr {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx, pad[3];
} Hdr;
_Static_assert(sizeof(Hdr) == 24, "virtio_gpu_ctrl_hdr");

typedef struct GpuRect { uint32_t x, y, w, h; } GpuRect;

/* One pixel of the resource's format as the display's B8G8R8X8. */
static inline uint32_t to_display(uint32_t format, uint32_t p) {
    switch (format) {
    case B8G8R8A8: case B8G8R8X8: return p;                                     /* bytes B G R x: as it is */
    case A8R8G8B8: case X8R8G8B8: return __builtin_bswap32(p);                  /* bytes x R G B */
    case R8G8B8A8: case R8G8B8X8: return (p & 0xff00ff00u) | (p >> 16 & 0xff) | (p & 0xff) << 16; /* R G B x */
    case A8B8G8R8: case X8B8G8R8: { uint32_t q = __builtin_bswap32(p); return (q & 0xff00ff00u) | (q >> 16 & 0xff) | (q & 0xff) << 16; }
    default: return p;
    }
}

#endif
