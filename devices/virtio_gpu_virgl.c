// SPDX-License-Identifier: GPL-2.0-or-later
/* virtio-gpu 3D: the guest's OpenGL (Mesa's virgl driver) runs on the host's GPU through
   virglrenderer, which turns the guest's Gallium command stream into OpenGL ES calls, here
   on ANGLE's EGL (OpenGL ES on Metal). Both are loaded at run time from a folder of frameworks
   (HV_VIRGL), so nothing is linked against them.

   GL contexts belong to a thread: one worker thread owns them and runs every request. CPU threads
   hand requests over (notify), and CPU 0's tick hands finished ones back (hv_virtio_gpu_poll), as
   virtio-blk does. A fenced request is finished when virglrenderer says its fence has passed. */
#include "virtio_gpu_virgl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>

#include "virtio_gpu_priv.h"

/* --- virglrenderer's interface (virglrenderer.h, MIT), what is used of it --- */
struct virgl_box { uint32_t x, y, z, w, h, d; };
struct virgl_renderer_resource_create_args {
    uint32_t handle, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
};
struct virgl_renderer_gl_ctx_param { int version; bool shared; int major_ver, minor_ver, compat_ctx; };
struct virgl_renderer_callbacks {
    int version;
    void (*write_fence)(void *cookie, uint32_t fence);
    void *(*create_gl_context)(void *cookie, int scanout_idx, struct virgl_renderer_gl_ctx_param *param);
    void (*destroy_gl_context)(void *cookie, void *ctx);
    int (*make_current)(void *cookie, int scanout_idx, void *ctx);
    int (*get_drm_fd)(void *cookie);
    void (*write_context_fence)(void *cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id);
    int (*get_server_fd)(void *cookie, uint32_t version);
    void *(*get_egl_display)(void *cookie);
};
#define VIRGL_RENDERER_USE_GLES (1 << 4)
#define VIRGL_RENDERER_NATIVE_SHARE_TEXTURE (1 << 12) /* scanout resources as Metal textures (what UTM uses) */
#define PIPE_TEXTURE_2D 2
#define PIPE_BIND_RENDER_TARGET (1 << 1)
#define VIRGL_BIND_SCANOUT (1 << 18)
#define VIRGL_BIND_SHARED (1 << 20)
#define RESOURCE_FLAG_Y_0_TOP 1

static struct {
    int (*init)(void *, int, struct virgl_renderer_callbacks *);
    void (*get_cap_set)(uint32_t, uint32_t *, uint32_t *);
    void (*fill_caps)(uint32_t, uint32_t, void *);
    int (*context_create)(uint32_t, uint32_t, const char *);
    void (*context_destroy)(uint32_t);
    void (*ctx_attach_resource)(int, int);
    void (*ctx_detach_resource)(int, int);
    int (*resource_create)(struct virgl_renderer_resource_create_args *, struct iovec *, uint32_t);
    void (*resource_unref)(uint32_t);
    int (*resource_attach_iov)(int, struct iovec *, int);
    void (*resource_detach_iov)(int, struct iovec **, int *);
    int (*transfer_write_iov)(uint32_t, uint32_t, int, uint32_t, uint32_t, struct virgl_box *, uint64_t, struct iovec *, unsigned);
    int (*transfer_read_iov)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, struct virgl_box *, uint64_t, struct iovec *, int);
    int (*submit_cmd)(void *, int, int);
    int (*create_fence)(int, uint32_t);
    void (*poll)(void);
    void *(*get_cursor_data)(uint32_t, uint32_t *, uint32_t *);
} vr;

/* --- EGL (ANGLE), what is used of it --- */
typedef void *EGLDisplay, *EGLConfig, *EGLContext;
typedef int32_t EGLint;
static struct {
    EGLDisplay (*GetDisplay)(void *);
    unsigned (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    unsigned (*BindAPI)(unsigned);
    unsigned (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    unsigned (*DestroyContext)(EGLDisplay, EGLContext);
    unsigned (*MakeCurrent)(EGLDisplay, void *, void *, EGLContext);
} egl;
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB

#define RING 256

typedef struct Req {
    HvVirtqElem e;
    int q;
    uint32_t written;
    int fenced;              /* waits for fence before it is done */
    uint32_t fence;
    int done, pushed;
} Req;

typedef struct Res {
    uint32_t id, format, w, h;
    struct iovec *iov;       /* its backing, guest RAM */
    int niov;
    struct { uint64_t addr; uint32_t len; } *ents;
    struct Res *next;
} Res;

struct HvGpuVirgl {
    HvVirtioGpu *g;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Req *ring;
    uint32_t submitted, taken, published; /* submitted: by notify; taken: run by the worker; published: back to the guest */
    int started;                          /* 1 up, -1 failed */
    uint32_t last_fence;                  /* the last fence virglrenderer says has passed */
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext main_ctx;
    Res *res;
    uint32_t scanout_res;
    uint32_t *readback;
    size_t readback_px;
    char dir[1024];
};

static void *load(const char *dir, const char *rel, int global) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    void *h = dlopen(path, RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
    if (!h) fprintf(stderr, "hengevm: virgl: %s\n", dlerror());
    return h;
}

static void *cb_create_context(void *cookie, int scanout, struct virgl_renderer_gl_ctx_param *p) {
    HvGpuVirgl *v = cookie;
    (void)scanout;
    EGLint a[] = {EGL_CONTEXT_MAJOR_VERSION, p->major_ver, EGL_CONTEXT_MINOR_VERSION, p->minor_ver, EGL_NONE};
    /* Every context in one share group, the first one's included (it asks for no sharing): resources are made in
       virglrenderer's context 0 and used in the guest's, which see none of them from another group. */
    return egl.CreateContext(v->dpy, v->cfg, v->main_ctx, a);
}
static void cb_destroy_context(void *cookie, void *ctx) { egl.DestroyContext(((HvGpuVirgl *)cookie)->dpy, ctx); }
static int cb_make_current(void *cookie, int scanout, void *ctx) {
    (void)scanout;
    return egl.MakeCurrent(((HvGpuVirgl *)cookie)->dpy, NULL, NULL, ctx) ? 0 : -1;
}
static void *cb_egl_display(void *cookie) { return ((HvGpuVirgl *)cookie)->dpy; }
static void cb_write_fence(void *cookie, uint32_t fence) { ((HvGpuVirgl *)cookie)->last_fence = fence; }

static struct virgl_renderer_callbacks callbacks = {
    .version = 4, .write_fence = cb_write_fence, .create_gl_context = cb_create_context,
    .destroy_gl_context = cb_destroy_context, .make_current = cb_make_current, .get_egl_display = cb_egl_display,
};

static void virgl_log(int level, const char *msg, void *data) {
    (void)data;
    fprintf(stderr, "virgl[%d]: %s", level, msg);
}

/* On the worker: EGL, then virglrenderer. */
static int bring_up(HvGpuVirgl *v) {
    const char *d = v->dir;
    /* ANGLE on Metal: its default on a Mac is OpenGL, which lacks the EGL images virglrenderer needs (as UTM sets). */
    setenv("ANGLE_DEFAULT_PLATFORM", "metal", 0);
    void *e = load(d, "EGL.framework/EGL", 1);
    if (!e || !load(d, "GLESv2.framework/GLESv2", 1) || !load(d, "epoxy.0.framework/epoxy.0", 1)) return -1;
    load(d, "vulkan.1.framework/vulkan.1", 1); /* virglrenderer links it (for Venus, unused here) */
    void *r = load(d, "virglrenderer.1.framework/virglrenderer.1", 0);
    if (!r) return -1;
#define E(n) if (!(*(void **)&egl.n = dlsym(e, "egl" #n))) { fprintf(stderr, "hengevm: virgl: no egl" #n "\n"); return -1; }
    E(GetDisplay) E(Initialize) E(BindAPI) E(ChooseConfig) E(CreateContext) E(DestroyContext) E(MakeCurrent)
#undef E
#define V(n) if (!(*(void **)&vr.n = dlsym(r, "virgl_renderer_" #n))) { fprintf(stderr, "hengevm: virgl: no virgl_renderer_" #n "\n"); return -1; }
    V(init) V(get_cap_set) V(fill_caps) V(context_create) V(context_destroy) V(ctx_attach_resource) V(ctx_detach_resource)
    V(resource_create) V(resource_unref) V(resource_attach_iov) V(resource_detach_iov) V(transfer_write_iov)
    V(transfer_read_iov) V(submit_cmd) V(create_fence) V(poll) V(get_cursor_data)
#undef V
    v->dpy = egl.GetDisplay(NULL);
    EGLint major, minor, n = 0;
    if (!v->dpy || !egl.Initialize(v->dpy, &major, &minor)) { fprintf(stderr, "hengevm: virgl: no EGL display\n"); return -1; }
    egl.BindAPI(EGL_OPENGL_ES_API);
    EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    if (!egl.ChooseConfig(v->dpy, want, &v->cfg, 1, &n) || n < 1) { fprintf(stderr, "hengevm: virgl: no EGL config\n"); return -1; }
    EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    v->main_ctx = egl.CreateContext(v->dpy, v->cfg, NULL, ctx);
    if (!v->main_ctx || !egl.MakeCurrent(v->dpy, NULL, NULL, v->main_ctx)) { fprintf(stderr, "hengevm: virgl: no GLES 3 context\n"); return -1; }
    void (*set_log)(void (*)(int, const char *, void *), void *, void (*)(void *)) = dlsym(r, "virgl_set_log_callback");
    if (set_log && getenv("VREND_DEBUG")) set_log(virgl_log, NULL, NULL);
    int rc = vr.init(v, VIRGL_RENDERER_USE_GLES | VIRGL_RENDERER_NATIVE_SHARE_TEXTURE, &callbacks);
    if (rc) { fprintf(stderr, "hengevm: virgl: virgl_renderer_init failed (%d)\n", rc); return -1; }
    fprintf(stderr, "hengevm: virgl: up, EGL %d.%d (ANGLE)\n", major, minor);
    return 0;
}

static Res *find(HvGpuVirgl *v, uint32_t id) {
    for (Res *r = v->res; r; r = r->next) if (r->id == id) return r;
    return NULL;
}

static void detach(HvGpuVirgl *v, Res *r) {
    if (!r->iov) return;
    struct iovec *iov = NULL;
    int n = 0;
    vr.resource_detach_iov((int)r->id, &iov, &n);
    free(r->iov), free(r->ents);
    r->iov = NULL, r->ents = NULL, r->niov = 0;
    (void)v;
}

static void unref(HvGpuVirgl *v, uint32_t id) {
    for (Res **p = &v->res; *p; p = &(*p)->next)
        if ((*p)->id == id) {
            Res *r = *p;
            *p = r->next;
            detach(v, r);
            vr.resource_unref(id);
            if (v->scanout_res == id) v->scanout_res = 0;
            free(r);
            return;
        }
}

static uint32_t create(HvGpuVirgl *v, struct virgl_renderer_resource_create_args *a) {
    if (!a->handle || find(v, a->handle)) return RESP_ERR_INVALID_RESOURCE_ID;
    Res *r = calloc(1, sizeof *r);
    if (!r) return RESP_ERR_OUT_OF_MEMORY;
    /* Henge reads the scanout back rather than sharing it with the host, so nothing is shared: without SCANOUT and
       SHARED a resource is an ordinary GL texture. With them virglrenderer backs it by an external image, and stores
       an alpha-less one (B8G8R8X8, which Linux scans out) as RGB8, which ANGLE can neither render to nor read back. */
    a->bind &= ~(VIRGL_BIND_SCANOUT | VIRGL_BIND_SHARED);
    if (vr.resource_create(a, NULL, 0)) { free(r); return RESP_ERR_UNSPEC; }
    r->id = a->handle, r->format = a->format, r->w = a->width, r->h = a->height;
    r->next = v->res;
    v->res = r;
    return RESP_OK_NODATA;
}

/* The scanout's resource, the flushed rectangle of it, onto the display. */
static uint32_t flush(HvGpuVirgl *v, const uint8_t *req) {
    HvVirtioGpu *g = v->g;
    Rect rc;
    uint32_t id;
    memcpy(&rc, req + 24, 16);
    memcpy(&id, req + 40, 4);
    Res *r = find(v, id);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (id != v->scanout_res) return RESP_OK_NODATA;
    uint32_t w = r->w < g->width ? r->w : g->width, h = r->h < g->height ? r->h : g->height;
    if (rc.x >= w || rc.y >= h) return RESP_OK_NODATA;
    if (rc.w > w - rc.x) rc.w = w - rc.x;
    if (rc.h > h - rc.y) rc.h = h - rc.y;
    size_t px = (size_t)rc.w * rc.h;
    if (px > v->readback_px) {
        free(v->readback);
        v->readback = malloc(px * 4);
        v->readback_px = v->readback ? px : 0;
        if (!v->readback) return RESP_ERR_OUT_OF_MEMORY;
    }
    struct virgl_box box = {rc.x, rc.y, 0, rc.w, rc.h, 1};
    struct iovec iov = {v->readback, px * 4};
    int err = vr.transfer_read_iov(id, 0, 0, rc.w * 4, 0, &box, 0, &iov, 1);
    static unsigned failures;
    if (getenv("HV_GPU_LOG") || (err && failures++ < 20))
        fprintf(stderr, "hengevm: virgl: flush res %u (%ux%u fmt %u) rect %u,%u %ux%u -> %d\n", id, r->w, r->h,
                r->format, rc.x, rc.y, rc.w, rc.h, err);
    if (err) return RESP_ERR_UNSPEC;
    for (uint32_t y = 0; y < rc.h; y++) {
        const uint32_t *src = v->readback + (size_t)y * rc.w;
        uint32_t *dst = g->display + (size_t)(rc.y + y) * g->width + rc.x;
        if (r->format == B8G8R8A8 || r->format == B8G8R8X8) memcpy(dst, src, (size_t)rc.w * 4);
        else for (uint32_t x = 0; x < rc.w; x++) dst[x] = to_display(r->format, src[x]);
    }
    g->flushes++;
    g->changes++;
    return RESP_OK_NODATA;
}

/* One control request; the response goes into the element now, and back to the guest when done. */
static uint32_t control(HvGpuVirgl *v, Req *q) {
    HvVirtioGpu *g = v->g;
    const HvVirtqElem *e = &q->e;
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
        uint32_t mode[6] = {0, 0, g->width, g->height, 1, 0};
        memcpy(resp + 24, mode, sizeof mode);
        resp_len = 24 + MAX_SCANOUTS * 24;
        break;
    }
    case CMD_GET_CAPSET_INFO: {
        uint32_t index;
        memcpy(&index, req + 24, 4);
        uint32_t info[4] = {0};
        if (index < 2) {
            info[0] = index + 1; /* VIRGL, VIRGL2 */
            vr.get_cap_set(info[0], &info[1], &info[2]);
        }
        memcpy(resp + 24, info, sizeof info);
        type = RESP_OK_CAPSET_INFO;
        resp_len = 40;
        break;
    }
    case CMD_GET_CAPSET: {
        uint32_t id, version, max_ver = 0, max_size = 0;
        memcpy(&id, req + 24, 4);
        memcpy(&version, req + 28, 4);
        vr.get_cap_set(id, &max_ver, &max_size);
        if (!max_size || hv_virtq_writable(e) < 24 + max_size) { type = RESP_ERR_INVALID_PARAMETER; break; }
        uint8_t *out = calloc(1, 24 + max_size);
        if (!out) { type = RESP_ERR_OUT_OF_MEMORY; break; }
        vr.fill_caps(id, version, out + 24);
        Hdr o = {.type = RESP_OK_CAPSET, .flags = h.flags & FLAG_FENCE, .fence_id = h.fence_id, .ctx_id = h.ctx_id};
        memcpy(out, &o, sizeof o);
        uint32_t w = hv_virtq_write(&g->vio, e, 0, out, 24 + max_size);
        free(out);
        if (h.flags & FLAG_FENCE) q->fenced = 1, q->fence = (uint32_t)h.fence_id;
        return w;
    }
    case CMD_RESOURCE_CREATE_2D: {
        uint32_t c[4]; /* resource_id, format, width, height */
        memcpy(c, req + 24, 16);
        struct virgl_renderer_resource_create_args a = {
            .handle = c[0], .target = PIPE_TEXTURE_2D, .format = c[1], .bind = PIPE_BIND_RENDER_TARGET,
            .width = c[2], .height = c[3], .depth = 1, .array_size = 1, .flags = RESOURCE_FLAG_Y_0_TOP};
        type = create(v, &a);
        break;
    }
    case CMD_RESOURCE_CREATE_3D: {
        uint32_t c[11]; /* resource_id, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags */
        memcpy(c, req + 24, sizeof c);
        struct virgl_renderer_resource_create_args a = {
            .handle = c[0], .target = c[1], .format = c[2], .bind = c[3], .width = c[4], .height = c[5],
            .depth = c[6], .array_size = c[7], .last_level = c[8], .nr_samples = c[9], .flags = c[10]};
        type = create(v, &a);
        if (getenv("HV_GPU_LOG"))
            fprintf(stderr, "gpu3d: create %u target %u format %u bind %#x %ux%ux%u array %u levels %u samples %u flags %#x -> %#x\n",
                    c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], type);
        break;
    }
    case CMD_RESOURCE_UNREF: {
        uint32_t id;
        memcpy(&id, req + 24, 4);
        if (!find(v, id)) type = RESP_ERR_INVALID_RESOURCE_ID;
        else unref(v, id);
        break;
    }
    case CMD_SET_SCANOUT: {
        uint32_t scanout, id;
        memcpy(&scanout, req + 40, 4);
        memcpy(&id, req + 44, 4);
        if (scanout != 0) { type = RESP_ERR_INVALID_SCANOUT_ID; break; }
        if (id && !find(v, id)) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        v->scanout_res = id;
        g->scanout_res = id;
        if (!id) memset(g->display, 0, (size_t)g->width * g->height * 4);
        break;
    }
    case CMD_RESOURCE_FLUSH: type = flush(v, req); break;
    case CMD_TRANSFER_TO_HOST_2D: {
        Rect r;
        uint64_t offset;
        uint32_t id;
        memcpy(&r, req + 24, 16);
        memcpy(&offset, req + 40, 8);
        memcpy(&id, req + 48, 4);
        if (!find(v, id)) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        struct virgl_box box = {r.x, r.y, 0, r.w, r.h, 1};
        if (vr.transfer_write_iov(id, 0, 0, 0, 0, &box, offset, NULL, 0)) type = RESP_ERR_UNSPEC;
        break;
    }
    case CMD_TRANSFER_TO_HOST_3D:
    case CMD_TRANSFER_FROM_HOST_3D: {
        uint32_t b[6], id, level, stride, layer_stride;
        uint64_t offset;
        memcpy(b, req + 24, 24);
        memcpy(&offset, req + 48, 8);
        memcpy(&id, req + 56, 4);
        memcpy(&level, req + 60, 4);
        memcpy(&stride, req + 64, 4);
        memcpy(&layer_stride, req + 68, 4);
        Res *r = find(v, id);
        if (!r) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        struct virgl_box box = {b[0], b[1], b[2], b[3], b[4], b[5]};
        int rc = h.type == CMD_TRANSFER_TO_HOST_3D
                     ? vr.transfer_write_iov(id, h.ctx_id, (int)level, stride, layer_stride, &box, offset, NULL, 0)
                     : vr.transfer_read_iov(id, h.ctx_id, level, stride, layer_stride, &box, offset, NULL, 0);
        if (getenv("HV_GPU_LOG"))
            fprintf(stderr, "gpu3d: %s res %u ctx %u box %u,%u,%u %ux%ux%u offset %llu level %u stride %u layer %u -> %d\n",
                    h.type == CMD_TRANSFER_TO_HOST_3D ? "to-host" : "from-host", id, h.ctx_id, b[0], b[1], b[2], b[3], b[4], b[5],
                    (unsigned long long)offset, level, stride, layer_stride, rc);
        if (rc) type = RESP_ERR_UNSPEC;
        else if (h.type == CMD_TRANSFER_FROM_HOST_3D)
            for (int i = 0; i < r->niov; i++) hv_dma_written(g->m, r->ents[i].addr, r->ents[i].len);
        break;
    }
    case CMD_RESOURCE_ATTACH_BACKING: {
        uint32_t id, nents;
        memcpy(&id, req + 24, 4);
        memcpy(&nents, req + 28, 4);
        Res *r = find(v, id);
        if (!r) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        if (!nents || nents > 1u << 20) { type = RESP_ERR_INVALID_PARAMETER; break; }
        detach(v, r);
        r->iov = calloc(nents, sizeof *r->iov);
        r->ents = calloc(nents, sizeof *r->ents);
        if (!r->iov || !r->ents) { free(r->iov), free(r->ents); r->iov = NULL, r->ents = NULL; type = RESP_ERR_OUT_OF_MEMORY; break; }
        for (uint32_t i = 0; i < nents; i++) {
            uint8_t ent[16];
            uint64_t addr;
            uint32_t len;
            if (hv_virtq_read(&g->vio, e, 32 + i * 16, ent, 16) != 16) { type = RESP_ERR_INVALID_PARAMETER; break; }
            memcpy(&addr, ent, 8);
            memcpy(&len, ent + 8, 4);
            void *p = hv_phys_ptr(g->m, addr, len);
            if (!p) { type = RESP_ERR_INVALID_PARAMETER; break; }
            r->iov[i] = (struct iovec){p, len};
            r->ents[i].addr = addr, r->ents[i].len = len;
        }
        if (type != RESP_OK_NODATA || vr.resource_attach_iov((int)id, r->iov, (int)nents)) {
            free(r->iov), free(r->ents);
            r->iov = NULL, r->ents = NULL;
            if (type == RESP_OK_NODATA) type = RESP_ERR_UNSPEC;
            break;
        }
        r->niov = (int)nents;
        break;
    }
    case CMD_RESOURCE_DETACH_BACKING: {
        uint32_t id;
        memcpy(&id, req + 24, 4);
        Res *r = find(v, id);
        if (!r) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        detach(v, r);
        break;
    }
    case CMD_CTX_CREATE: {
        uint32_t nlen;
        char name[65] = {0};
        memcpy(&nlen, req + 24, 4);
        if (nlen > 64) nlen = 64;
        memcpy(name, req + 32, nlen);
        if (vr.context_create(h.ctx_id, nlen, name)) type = RESP_ERR_UNSPEC;
        break;
    }
    case CMD_CTX_DESTROY: vr.context_destroy(h.ctx_id); break;
    case CMD_CTX_ATTACH_RESOURCE:
    case CMD_CTX_DETACH_RESOURCE: {
        uint32_t id;
        memcpy(&id, req + 24, 4);
        if (h.type == CMD_CTX_ATTACH_RESOURCE) vr.ctx_attach_resource((int)h.ctx_id, (int)id);
        else vr.ctx_detach_resource((int)h.ctx_id, (int)id);
        break;
    }
    case CMD_SUBMIT_3D: {
        uint32_t size;
        memcpy(&size, req + 24, 4);
        if (size & 3) { type = RESP_ERR_INVALID_PARAMETER; break; }
        void *buf = malloc(size ? size : 4);
        if (!buf) { type = RESP_ERR_OUT_OF_MEMORY; break; }
        if (hv_virtq_read(&g->vio, e, 32, buf, size) != size) type = RESP_ERR_INVALID_PARAMETER;
        else if (vr.submit_cmd(buf, (int)h.ctx_id, (int)(size / 4))) type = RESP_ERR_UNSPEC;
        free(buf);
        break;
    }
    default:
        if (g->m->log) fprintf(stderr, "hengevm: virtio-gpu (virgl): command %#x not supported\n", h.type);
        type = RESP_ERR_UNSPEC;
    }
    static int log = -1;
    if (log < 0) log = getenv("HV_GPU_LOG") != NULL;
    if (log) fprintf(stderr, "gpu3d: cmd %#x ctx %u (%u bytes in) -> %#x%s\n", h.type, h.ctx_id, n, type,
                     h.flags & FLAG_FENCE ? " fenced" : "");
    Hdr out = {.type = type, .flags = h.flags & FLAG_FENCE, .fence_id = h.flags & FLAG_FENCE ? h.fence_id : 0,
               .ctx_id = h.ctx_id, .ring_idx = h.ring_idx};
    memcpy(resp, &out, sizeof out);
    if (h.flags & FLAG_FENCE) q->fenced = 1, q->fence = (uint32_t)h.fence_id;
    return hv_virtq_write(&g->vio, e, 0, resp, resp_len);
}

static void cursor(HvGpuVirgl *v, const HvVirtqElem *e) {
    HvVirtioGpu *g = v->g;
    uint8_t req[56] = {0};
    if (hv_virtq_read(&g->vio, e, 0, req, sizeof req) < 48) return;
    Hdr h;
    memcpy(&h, req, sizeof h);
    uint32_t pos[4], id, hot[2];
    memcpy(pos, req + 24, 16);
    memcpy(&id, req + 40, 4);
    memcpy(hot, req + 44, 8);
    g->cursor_x = (int32_t)pos[1], g->cursor_y = (int32_t)pos[2];
    g->changes++;
    if (h.type != CMD_UPDATE_CURSOR) return;
    g->cursor_on = 0;
    if (!id || !find(v, id)) return;
    uint32_t w = 0, hh = 0;
    uint32_t *px = vr.get_cursor_data(id, &w, &hh);
    if (!px) return;
    g->hot_x = (int32_t)hot[0], g->hot_y = (int32_t)hot[1];
    memset(g->cursor, 0, sizeof g->cursor);
    for (uint32_t y = 0; y < 64 && y < hh; y++)
        for (uint32_t x = 0; x < 64 && x < w; x++) g->cursor[y * 64 + x] = px[(size_t)y * w + x]; /* B G R A: alpha on top */
    free(px);
    g->cursor_on = 1;
}

static int fence_passed(HvGpuVirgl *v, uint32_t f) { return (int32_t)(v->last_fence - f) >= 0; }

static void *worker(void *arg) {
    HvGpuVirgl *v = arg;
    int ok = bring_up(v) == 0;
    pthread_mutex_lock(&v->mu);
    v->started = ok ? 1 : -1;
    pthread_cond_broadcast(&v->cv);
    pthread_mutex_unlock(&v->mu);
    if (!ok) return NULL;
    for (;;) {
        pthread_mutex_lock(&v->mu);
        int waiting = 0;
        for (uint32_t i = v->published; i != v->taken; i++) {
            Req *q = &v->ring[i % RING];
            if (q->fenced && !__atomic_load_n(&q->done, __ATOMIC_ACQUIRE)) waiting = 1;
        }
        if (v->taken == v->submitted) {
            if (waiting) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 500000;
                if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
                pthread_cond_timedwait(&v->cv, &v->mu, &ts);
            } else pthread_cond_wait(&v->cv, &v->mu);
        }
        uint32_t upto = __atomic_load_n(&v->submitted, __ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&v->mu);
        int finished = 0;
        while (v->taken != upto) {
            Req *q = &v->ring[v->taken % RING];
            q->fenced = 0;
            if (q->q == CONTROLQ) q->written = control(v, q);
            else cursor(v, &q->e), q->written = 0;
            if (q->fenced) vr.create_fence((int)q->fence, 0);
            else __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE), finished = 1;
            v->taken++;
        }
        vr.poll();
        for (uint32_t i = v->published; i != v->taken; i++) {
            Req *q = &v->ring[i % RING];
            if (q->fenced && !q->done && fence_passed(v, q->fence)) __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE), finished = 1;
        }
        if (finished) hv_machine_wake(v->g->m);
    }
    return NULL;
}

/* CPU thread, device lock held: hand what the driver made available to the worker, while the ring has room. */
static void notify(HvVirtio *vio, int qi) {
    HvVirtioGpu *g = vio->dev;
    HvGpuVirgl *v = g->virgl;
    int any = 0;
    while (v->submitted - v->published < RING) {
        Req *q = &v->ring[v->submitted % RING];
        if (hv_virtq_pop(vio, qi, &q->e) <= 0) break;
        q->q = qi;
        q->done = 0;
        pthread_mutex_lock(&v->mu);
        __atomic_store_n(&v->submitted, v->submitted + 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&v->mu);
        any = 1;
    }
    if (any) pthread_cond_signal(&v->cv);
}

void hv_gpu_virgl_poll(HvGpuVirgl *v) {
    if (!v) return;
    HvVirtio *vio = &v->g->vio;
    int any = 0, last_q = 0;
    for (uint32_t i = v->published; i != v->submitted; i++) {
        Req *q = &v->ring[i % RING];
        if (q->pushed || !__atomic_load_n(&q->done, __ATOMIC_ACQUIRE)) continue;
        hv_virtq_push(vio, q->q, &q->e, q->written);
        q->pushed = 1;
        any = 1, last_q = q->q;
    }
    while (v->published != v->submitted && v->ring[v->published % RING].pushed) {
        Req *q = &v->ring[v->published % RING];
        q->pushed = 0;
        __atomic_store_n(&q->done, 0, __ATOMIC_RELAXED);
        v->published++;
    }
    if (!any) return;
    hv_virtio_interrupt(vio);
    notify(vio, last_q == CONTROLQ ? CURSORQ : CONTROLQ); /* requests that waited for room */
    notify(vio, last_q);
}

HvGpuVirgl *hv_gpu_virgl_start(HvVirtioGpu *g, const char *dir) {
    HvGpuVirgl *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->g = g;
    snprintf(v->dir, sizeof v->dir, "%s", dir);
    v->ring = calloc(RING, sizeof *v->ring);
    pthread_mutex_init(&v->mu, NULL);
    pthread_cond_init(&v->cv, NULL);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20); /* shader compilers recurse deeply */
    if (!v->ring || pthread_create(&v->thread, &at, worker, v) != 0) { free(v->ring), free(v); return NULL; }
    pthread_mutex_lock(&v->mu);
    while (!v->started) pthread_cond_wait(&v->cv, &v->mu);
    int ok = v->started > 0;
    pthread_mutex_unlock(&v->mu);
    if (!ok) { pthread_join(v->thread, NULL); free(v->ring), free(v); return NULL; } /* the libraries stay loaded */
    return v;
}

void hv_gpu_virgl_attach(HvGpuVirgl *v) { v->g->vio.notify = notify; }
