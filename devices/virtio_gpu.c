// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    CMD_UPDATE_CURSOR = 0x0300,
    CMD_MOVE_CURSOR,
    RESP_OK_NODATA = 0x1100,
    RESP_OK_DISPLAY_INFO,
    RESP_ERR_UNSPEC = 0x1200,
    RESP_ERR_OUT_OF_MEMORY,
    RESP_ERR_INVALID_SCANOUT_ID,
    RESP_ERR_INVALID_RESOURCE_ID,
    RESP_ERR_INVALID_CONTEXT_ID,
    RESP_ERR_INVALID_PARAMETER,
};
#define FLAG_FENCE 1u
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

typedef struct Rect { uint32_t x, y, w, h; } Rect;

struct HvGpuResource {
    uint32_t id, format, w, h;
    uint32_t *px;                 /* w * h, as the guest's format */
    int nents;
    struct { uint64_t addr; uint32_t len; } *ents;
    HvGpuResource *next;
};

static HvGpuResource *find(HvVirtioGpu *g, uint32_t id) {
    for (HvGpuResource *r = g->res; r; r = r->next) if (r->id == id) return r;
    return NULL;
}

static void unref(HvVirtioGpu *g, uint32_t id) {
    for (HvGpuResource **p = &g->res; *p; p = &(*p)->next)
        if ((*p)->id == id) {
            HvGpuResource *r = *p;
            *p = r->next;
            if (g->scanout_res == id) g->scanout_res = 0;
            free(r->px), free(r->ents), free(r);
            return;
        }
}

/* Bytes [off, off + len) of the resource's backing (guest memory), into dst. */
static int read_backing(HvVirtioGpu *g, HvGpuResource *r, uint64_t off, uint8_t *dst, uint64_t len) {
    for (int i = 0; i < r->nents && len; i++) {
        uint64_t elen = r->ents[i].len;
        if (off >= elen) { off -= elen; continue; }
        uint64_t take = elen - off < len ? elen - off : len;
        uint8_t *src = hv_phys_ptr(g->m, r->ents[i].addr + off, take);
        if (src) memcpy(dst, src, take);
        else if (hv_phys_read(g->m, r->ents[i].addr + off, dst, (unsigned)take) != 0) return -1;
        dst += take, len -= take, off = 0;
    }
    return len ? -1 : 0;
}

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

static uint32_t cmd_transfer(HvVirtioGpu *g, const uint8_t *req) {
    Rect r;
    uint64_t offset;
    uint32_t id;
    memcpy(&r, req + 24, 16);
    memcpy(&offset, req + 40, 8);
    memcpy(&id, req + 48, 4);
    HvGpuResource *res = find(g, id);
    if (!res) return RESP_ERR_INVALID_RESOURCE_ID;
    if (!res->nents) return RESP_ERR_UNSPEC;
    if (r.x > res->w || r.y > res->h || r.w > res->w - r.x || r.h > res->h - r.y) return RESP_ERR_INVALID_PARAMETER;
    uint64_t stride = (uint64_t)res->w * 4;
    if (!offset && !r.x && !r.y && r.w == res->w) {
        if (read_backing(g, res, 0, (uint8_t *)res->px, stride * r.h)) return RESP_ERR_UNSPEC;
    } else {
        for (uint32_t row = 0; row < r.h; row++)
            if (read_backing(g, res, offset + stride * row, (uint8_t *)(res->px + (uint64_t)(r.y + row) * res->w + r.x), (uint64_t)r.w * 4))
                return RESP_ERR_UNSPEC;
    }
    return RESP_OK_NODATA;
}

static uint32_t cmd_flush(HvVirtioGpu *g, const uint8_t *req) {
    Rect r;
    uint32_t id;
    memcpy(&r, req + 24, 16);
    memcpy(&id, req + 40, 4);
    HvGpuResource *res = find(g, id);
    if (!res) return RESP_ERR_INVALID_RESOURCE_ID;
    if (id != g->scanout_res) return RESP_OK_NODATA;
    uint32_t w = res->w < g->width ? res->w : g->width, h = res->h < g->height ? res->h : g->height;
    if (r.x >= w || r.y >= h) return RESP_OK_NODATA;
    if (r.w > w - r.x) r.w = w - r.x;
    if (r.h > h - r.y) r.h = h - r.y;
    for (uint32_t y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = res->px + (uint64_t)y * res->w + r.x;
        uint32_t *dst = g->display + (uint64_t)y * g->width + r.x;
        if (res->format == B8G8R8X8 || res->format == B8G8R8A8) memcpy(dst, src, (size_t)r.w * 4);
        else for (uint32_t x = 0; x < r.w; x++) dst[x] = to_display(res->format, src[x]);
    }
    g->flushes++;
    return RESP_OK_NODATA;
}

/* Handles one control request; returns how many response bytes went back. */
static uint32_t control(HvVirtioGpu *g, const HvVirtqElem *e) {
    uint8_t req[512] = {0};
    uint32_t n = hv_virtq_read(&g->vio, e, 0, req, sizeof req);
    Hdr h;
    memcpy(&h, req, sizeof h);
    uint8_t resp[24 + MAX_SCANOUTS * 24] = {0};
    uint32_t resp_len = 24, type = RESP_OK_NODATA;
    if (n < 24) type = RESP_ERR_UNSPEC;
    else switch (h.type) {
    case CMD_GET_DISPLAY_INFO: {
        type = RESP_OK_DISPLAY_INFO;
        uint32_t mode[6] = {0, 0, g->width, g->height, 1, 0}; /* rect, enabled, flags */
        memcpy(resp + 24, mode, sizeof mode);
        resp_len = 24 + MAX_SCANOUTS * 24;
        break;
    }
    case CMD_RESOURCE_CREATE_2D: {
        uint32_t c[4]; /* resource_id, format, width, height */
        memcpy(c, req + 24, 16);
        if (!c[0] || find(g, c[0])) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        if (!c[2] || !c[3] || c[2] > 16384 || c[3] > 16384) { type = RESP_ERR_INVALID_PARAMETER; break; }
        HvGpuResource *r = calloc(1, sizeof *r);
        if (r) r->px = calloc((size_t)c[2] * c[3], 4);
        if (!r || !r->px) { free(r); type = RESP_ERR_OUT_OF_MEMORY; break; }
        r->id = c[0], r->format = c[1], r->w = c[2], r->h = c[3];
        r->next = g->res;
        g->res = r;
        break;
    }
    case CMD_RESOURCE_UNREF: {
        uint32_t id;
        memcpy(&id, req + 24, 4);
        if (!find(g, id)) type = RESP_ERR_INVALID_RESOURCE_ID;
        else unref(g, id);
        break;
    }
    case CMD_SET_SCANOUT: {
        uint32_t scanout, id;
        memcpy(&scanout, req + 40, 4);
        memcpy(&id, req + 44, 4);
        if (scanout != 0) { type = RESP_ERR_INVALID_SCANOUT_ID; break; }
        if (id && !find(g, id)) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        g->scanout_res = id;
        if (!id) memset(g->display, 0, (size_t)g->width * g->height * 4);
        break;
    }
    case CMD_RESOURCE_FLUSH: type = cmd_flush(g, req); break;
    case CMD_TRANSFER_TO_HOST_2D: type = cmd_transfer(g, req); break;
    case CMD_RESOURCE_ATTACH_BACKING: {
        uint32_t id, nents;
        memcpy(&id, req + 24, 4);
        memcpy(&nents, req + 28, 4);
        HvGpuResource *r = find(g, id);
        if (!r) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        if (nents > 1u << 20) { type = RESP_ERR_INVALID_PARAMETER; break; }
        free(r->ents);
        r->ents = calloc(nents ? nents : 1, sizeof *r->ents);
        r->nents = 0;
        if (!r->ents) { type = RESP_ERR_OUT_OF_MEMORY; break; }
        /* The entries follow the request: {le64 addr, le32 length, le32 padding} each. */
        for (uint32_t i = 0; i < nents; i++) {
            uint8_t ent[16];
            if (hv_virtq_read(&g->vio, e, 32 + i * 16, ent, 16) != 16) { type = RESP_ERR_INVALID_PARAMETER; break; }
            memcpy(&r->ents[i].addr, ent, 8);
            memcpy(&r->ents[i].len, ent + 8, 4);
            r->nents++;
        }
        break;
    }
    case CMD_RESOURCE_DETACH_BACKING: {
        uint32_t id;
        memcpy(&id, req + 24, 4);
        HvGpuResource *r = find(g, id);
        if (!r) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        free(r->ents);
        r->ents = NULL, r->nents = 0;
        break;
    }
    default:
        if (g->m->log) fprintf(stderr, "hengevm: virtio-gpu: command %#x not supported\n", h.type);
        type = RESP_ERR_UNSPEC;
    }
    static int log = -1;
    if (log < 0) log = getenv("HV_GPU_LOG") != NULL;
    if (log) fprintf(stderr, "gpu: cmd %#x (%u bytes in, %u writable, %d segs) -> %#x, %u back\n", h.type, n,
                     hv_virtq_writable(e), e->nseg, type, resp_len);
    Hdr out = {.type = type, .flags = h.flags & FLAG_FENCE, .fence_id = h.flags & FLAG_FENCE ? h.fence_id : 0,
               .ctx_id = h.ctx_id, .ring_idx = h.ring_idx};
    memcpy(resp, &out, sizeof out);
    return hv_virtq_write(&g->vio, e, 0, resp, resp_len);
}

static void cursor(HvVirtioGpu *g, const HvVirtqElem *e) {
    uint8_t req[56] = {0};
    if (hv_virtq_read(&g->vio, e, 0, req, sizeof req) < 48) return;
    Hdr h;
    memcpy(&h, req, sizeof h);
    uint32_t pos[4], id, hot[2]; /* scanout_id, x, y, padding; resource_id; hot_x, hot_y */
    memcpy(pos, req + 24, 16);
    memcpy(&id, req + 40, 4);
    memcpy(hot, req + 44, 8);
    g->cursor_x = (int32_t)pos[1], g->cursor_y = (int32_t)pos[2];
    if (h.type != CMD_UPDATE_CURSOR) return;
    HvGpuResource *r = id ? find(g, id) : NULL;
    g->cursor_on = r != NULL;
    if (!r) return;
    g->hot_x = (int32_t)hot[0], g->hot_y = (int32_t)hot[1];
    memset(g->cursor, 0, sizeof g->cursor);
    for (uint32_t y = 0; y < 64 && y < r->h; y++)
        for (uint32_t x = 0; x < 64 && x < r->w; x++) {
            uint32_t p = r->px[(uint64_t)y * r->w + x];
            /* Keep the alpha: the cursor is blended over the picture. Alpha last in memory (B G R A, R G B A)
               is the top byte; first (A R G B, A B G R) the bottom one; the X formats are opaque. */
            uint32_t a;
            switch (r->format) {
            case B8G8R8A8: case R8G8B8A8: a = p >> 24; break;
            case A8R8G8B8: case A8B8G8R8: a = p & 0xff; break;
            default: a = 255;
            }
            g->cursor[y * 64 + x] = (to_display(r->format, p) & 0xffffff) | a << 24;
        }
}

static void notify(HvVirtio *v, int q) {
    HvVirtioGpu *g = v->dev;
    static HvVirtqElem e;
    int any = 0;
    while (hv_virtq_pop(v, q, &e) > 0) {
        uint32_t written = 0;
        if (q == CONTROLQ) written = control(g, &e);
        else if (q == CURSORQ) cursor(g, &e);
        hv_virtq_push(v, q, &e, written);
        any = 1;
    }
    if (any) hv_virtio_interrupt(v);
}

static void config_write(HvVirtio *v, uint32_t off, uint64_t val, unsigned size) {
    /* events_clear (+4): the driver acknowledges a display change; events_read (+0) is the device's. */
    if (off == 4 && size == 4) {
        uint32_t events;
        memcpy(&events, v->config, 4);
        events &= ~(uint32_t)val;
        memcpy(v->config, &events, 4);
    }
}

HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, uint32_t width, uint32_t height, uint32_t *display) {
    HvVirtioGpu *g = calloc(1, sizeof *g);
    HvVirtio *v = &g->vio;
    hv_virtio_init(v, m, 16, 0x038000, 2, 0); /* display controller, other; VERSION_1 only: no virgl, no EDID */
    v->dev = g;
    v->notify = notify;
    v->config_write = config_write;
    uint32_t cfg[4] = {0, 0, 1, 0}; /* events_read, events_clear, num_scanouts, num_capsets */
    memcpy(v->config, cfg, sizeof cfg);
    v->config_len = 16;
    g->m = m, g->width = width, g->height = height, g->display = display;
    hv_pcie_attach(bus, &v->pci);
    return g;
}

void hv_virtio_gpu_compose(HvVirtioGpu *g, uint32_t *out) {
    size_t n = (size_t)g->width * g->height;
    memcpy(out, g->display, n * 4);
    if (!g->cursor_on || !g->scanout_res) return;
    int32_t x0 = g->cursor_x - g->hot_x, y0 = g->cursor_y - g->hot_y;
    for (int y = 0; y < 64; y++) {
        int32_t dy = y0 + y;
        if (dy < 0 || dy >= (int32_t)g->height) continue;
        for (int x = 0; x < 64; x++) {
            int32_t dx = x0 + x;
            if (dx < 0 || dx >= (int32_t)g->width) continue;
            uint32_t c = g->cursor[y * 64 + x], a = c >> 24;
            if (!a) continue;
            uint32_t *d = &out[(size_t)dy * g->width + dx];
            if (a == 255) { *d = c | 0xff000000u; continue; }
            uint32_t o = *d, r = 0;
            for (int s = 0; s < 24; s += 8) {
                uint32_t cs = c >> s & 0xff, os = o >> s & 0xff;
                r |= ((cs * a + os * (255 - a)) / 255) << s;
            }
            *d = r | 0xff000000u;
        }
    }
}
