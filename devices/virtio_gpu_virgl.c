// SPDX-License-Identifier: GPL-2.0-or-later
/* virtio-gpu 3D: the guest's OpenGL (Mesa's virgl driver) runs on the host's GPU through
   virglrenderer, which turns the guest's Gallium command stream into OpenGL ES calls, here
   on ANGLE's EGL (OpenGL ES on Metal). Both are loaded at run time from a folder of frameworks
   (HV_VIRGL), so nothing is linked against them.

   GL contexts belong to a thread: one worker thread owns them and runs every request. CPU threads
   hand requests over (notify), and CPU 0's tick hands finished ones back (hv_virtio_gpu_poll), as
   virtio-blk does. A fenced request is finished when virglrenderer says its fence has passed. */
#include "virtio_gpu_virgl.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

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
#define VIRGL_RENDERER_RENDER_SERVER (1 << 9)        /* contexts of the newer protocols run in a render server */
#define VIRGL_RENDERER_NEPTUNE (1 << 15)             /* UTM's fork: Direct3D calls forwarded (src/neptune) */
#define VIRGL_RENDERER_FENCE_FLAG_MERGEABLE 1
#define VIRGL_NATIVE_HANDLE_METAL_TEXTURE 2
struct virgl_renderer_resource_create_blob_args {
    uint32_t res_handle, ctx_id, blob_mem, blob_flags;
    uint64_t blob_id, size;
    const struct iovec *iovecs;
    uint32_t num_iovs;
};
#define BLOB_MEM_GUEST 1
#define BLOB_MEM_HOST3D 2
#define PIPE_TEXTURE_2D 2
#define PIPE_BIND_RENDER_TARGET (1 << 1)
#define VIRGL_BIND_SCANOUT (1 << 18)
#define VIRGL_BIND_SHARED (1 << 20)
#define RESOURCE_FLAG_Y_0_TOP 1

struct virgl_renderer_resource_info {
    uint32_t handle, virgl_format, width, height, depth, flags, tex_id, stride;
    int drm_fourcc;
};

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
    void (*cleanup)(void *); /* optional */
    int (*resource_get_info)(int, struct virgl_renderer_resource_info *);
    void (*force_ctx_0)(void);
    /* a Direct3D device's (hv_virtio_gpu_create_d3d) */
    int (*context_create_with_flags)(uint32_t, uint32_t, uint32_t, const char *);
    int (*resource_create_blob)(const struct virgl_renderer_resource_create_blob_args *);
    int (*resource_map)(uint32_t, void **, uint64_t *);
    int (*resource_unmap)(uint32_t);
    int (*resource_map_fixed)(uint32_t, void *);
    int (*resource_get_map_info)(uint32_t, uint32_t *);
    int (*context_create_fence)(uint32_t, uint32_t, uint32_t, uint64_t);
    int (*create_handle_for_scanout)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void **);
    void (*release_handle_for_scanout)(int, void *);
    void (*reset)(void);
} vr;

int hv_metal_texture_read(void *texture, uint32_t *out, uint32_t max_w, uint32_t max_h, uint32_t *w, uint32_t *h); /* virtio_gpu_metal.m */
IOSurfaceRef hv_metal_texture_surface(void *texture);

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
    void *(*CreatePbufferFromClientBuffer)(EGLDisplay, unsigned, void *, EGLConfig, const EGLint *);
    unsigned (*DestroySurface)(EGLDisplay, void *);
    unsigned (*BindTexImage)(EGLDisplay, void *, EGLint);
    unsigned (*ReleaseTexImage)(EGLDisplay, void *, EGLint);
    unsigned (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
} egl;

/* --- OpenGL ES, what presenting uses of it (the rest is virglrenderer's business) --- */
static struct {
    void (*GenFramebuffers)(int, unsigned *);
    void (*DeleteFramebuffers)(int, const unsigned *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    void (*GenTextures)(int, unsigned *);
    void (*DeleteTextures)(int, const unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*GetIntegerv)(unsigned, int *);
    unsigned char (*IsEnabled)(unsigned);
    void (*Enable)(unsigned);
    void (*Disable)(unsigned);
    void (*Flush)(void);
    unsigned (*GetError)(void);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (*Finish)(void);
} gl;
#define EGL_IOSURFACE_ANGLE 0x3454
#define EGL_IOSURFACE_PLANE_ANGLE 0x345A
#define EGL_TEXTURE_TYPE_ANGLE 0x345C
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#define EGL_BIND_TO_TEXTURE_TARGET_ANGLE 0x348D
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_TEXTURE_FORMAT 0x3080
#define EGL_TEXTURE_TARGET 0x3081
#define EGL_TEXTURE_RGBA 0x305E
#define EGL_TEXTURE_2D 0x305F
#define EGL_BACK_BUFFER 0x3084
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_BGRA_EXT 0x80E1
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_NEAREST 0x2600
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
    int ringed;              /* the fence is on a context's timeline: ctx, ring, fence64 */
    uint32_t ctx;
    uint8_t ring;
    uint64_t fence64;
    uint32_t epoch;          /* which life of the device it was queued in (a reset starts another) */
    int done, pushed;
} Req;

typedef struct Res {
    uint32_t id, format, w, h;
    struct iovec *iov;       /* its backing, guest RAM */
    int niov;
    struct { uint64_t addr; uint32_t len; } *ents;
    /* A blob: memory of a size, the guest's or the host's. A host blob may be mapped into the shared region. */
    int blob;
    uint32_t blob_mem;
    uint64_t blob_size;
    int mapped;              /* its pages are in the shared region, from map_off */
    uint64_t map_off, map_len;
    /* As a scanout (SET_SCANOUT_BLOB): the picture's layout in the blob, and virglrenderer's texture of it. */
    uint32_t sw, sh, sformat, sstride, soffset;
    void *tex;
    IOSurfaceRef surf;       /* the texture's surface, if it has one: shown as it is (hv_gpu_virgl_surface) */
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
    int stop;                             /* hv_gpu_virgl_stop: the worker cleans up and ends */
    uint32_t last_fence;                  /* the last fence virglrenderer says has passed */
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext main_ctx;
    Res *res;
    uint32_t scanout_res;
    uint32_t *readback;
    size_t readback_px;
    /* Presenting without a readback (HV_VIRGL_PRESENT): each flush is a GPU copy of the scanout into one of two
       IOSurfaces, by turns, and the host shows the one finished last (hv_gpu_virgl_surface). */
    int present;                          /* 0 no; 1 a GPU blit into the surface; 2 read into the surface's memory */
    pthread_mutex_t present_mu;           /* the surfaces, between the worker and whoever shows or reads them */
    IOSurfaceRef surf[2];
    void *pbuf[2];
    unsigned surf_tex[2], surf_fbo[2], read_fbo;
    uint32_t surf_w, surf_h;
    int back, front;                      /* front: -1 until a frame is there */
    char dir[1024];
    /* A Direct3D device's. */
    uint32_t capset[4];
    uint32_t ncapsets;
    uint32_t epoch;                       /* bumped by each reset of the device */
    int reset_req;                        /* the worker is to let go of everything when it reaches reset_at */
    uint32_t reset_at;
    IOSurfaceRef scan_surf;               /* the scanout blob's surface (its Res's; under present_mu), or NULL */
};

static void hostmem_unmap(HvGpuVirgl *v, Res *r);

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
/* A fence on a context's timeline has passed (from virgl_renderer_poll, on the worker): the requests that
   waited for it or an earlier one on that timeline are done. */
static void cb_write_context_fence(void *cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id) {
    HvGpuVirgl *v = cookie;
    int any = 0;
    for (uint32_t i = v->published; i != v->taken; i++) {
        Req *q = &v->ring[i % RING];
        if (!q->fenced || !q->ringed || q->done || q->ctx != ctx_id || q->ring != ring_idx || q->fence64 > fence_id) continue;
        __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE);
        any = 1;
    }
    if (any) hv_machine_wake(v->g->m);
}
static int cb_server_fd(void *cookie, uint32_t version) {
    (void)cookie, (void)version;
    return -1; /* none of ours: virglrenderer starts its render server itself */
}

static struct virgl_renderer_callbacks callbacks = {
    .version = 4, .write_fence = cb_write_fence, .create_gl_context = cb_create_context,
    .destroy_gl_context = cb_destroy_context, .make_current = cb_make_current, .get_egl_display = cb_egl_display,
    .write_context_fence = cb_write_context_fence, .get_server_fd = cb_server_fd,
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
    void *gles = e ? load(d, "GLESv2.framework/GLESv2", 1) : NULL;
    if (!gles || !load(d, "epoxy.0.framework/epoxy.0", 1)) return -1;
    load(d, "vulkan.1.framework/vulkan.1", 1); /* virglrenderer links it (for Venus, unused here) */
    void *r = load(d, "virglrenderer.1.framework/virglrenderer.1", 0);
    if (!r) return -1;
#define E(n) if (!(*(void **)&egl.n = dlsym(e, "egl" #n))) { fprintf(stderr, "hengevm: virgl: no egl" #n "\n"); return -1; }
    E(GetDisplay) E(Initialize) E(BindAPI) E(ChooseConfig) E(CreateContext) E(DestroyContext) E(MakeCurrent)
#undef E
    /* Presenting: optional, so a missing piece only means the readback stays. */
    v->present = getenv("HV_VIRGL_PRESENT") ? atoi(getenv("HV_VIRGL_PRESENT")) : 0;
    if (v->present < 0 || v->present > 2) v->present = 0;
#define EO(n) if (!(*(void **)&egl.n = dlsym(e, "egl" #n))) v->present = 0;
    EO(CreatePbufferFromClientBuffer) EO(DestroySurface) EO(BindTexImage) EO(ReleaseTexImage) EO(GetConfigAttrib)
#undef EO
#define G(n) if (!(*(void **)&gl.n = dlsym(gles, "gl" #n))) v->present = 0;
    G(GenFramebuffers) G(DeleteFramebuffers) G(BindFramebuffer) G(FramebufferTexture2D) G(CheckFramebufferStatus)
    G(BlitFramebuffer) G(GenTextures) G(DeleteTextures) G(BindTexture) G(GetIntegerv) G(IsEnabled) G(Enable) G(Disable)
    G(Flush) G(GetError) G(ReadPixels) G(Finish)
#undef G
#define V(n) if (!(*(void **)&vr.n = dlsym(r, "virgl_renderer_" #n))) { fprintf(stderr, "hengevm: virgl: no virgl_renderer_" #n "\n"); return -1; }
    V(init) V(get_cap_set) V(fill_caps) V(context_create) V(context_destroy) V(ctx_attach_resource) V(ctx_detach_resource)
    V(resource_create) V(resource_unref) V(resource_attach_iov) V(resource_detach_iov) V(transfer_write_iov)
    V(transfer_read_iov) V(submit_cmd) V(create_fence) V(poll) V(get_cursor_data)
#undef V
    *(void **)&vr.cleanup = dlsym(r, "virgl_renderer_cleanup");
    *(void **)&vr.resource_get_info = dlsym(r, "virgl_renderer_resource_get_info");
    *(void **)&vr.force_ctx_0 = dlsym(r, "virgl_renderer_force_ctx_0");
    if (!vr.resource_get_info || !vr.force_ctx_0) v->present = 0;
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
    int flags = VIRGL_RENDERER_USE_GLES | VIRGL_RENDERER_NATIVE_SHARE_TEXTURE;
    if (v->g->d3d) {
        /* Direct3D: the guest's calls are carried out by dxmt-native (Direct3D 11 on Metal), in virglrenderer's
           render server: a process of its own on a Mac (UTM's, beside the frameworks), a thread on iOS. */
        char path[1300];
        snprintf(path, sizeof path, "%s/dxmt-native.framework/dxmt-native", d);
        setenv("NPT_D3D11_LIBRARY_PATH", path, 0), setenv("NPT_DXGI_LIBRARY_PATH", path, 0), setenv("NPT_D3D12_LIBRARY_PATH", path, 0);
        setenv("NPT_BACKEND", "dxmt", 0);
        snprintf(path, sizeof path, "%s/../macos-server/Contents/XPCServices/QEMUHelper.xpc/Contents/MacOS/QEMURenderServer.app/Contents/MacOS/QEMURenderServer", d);
        if (access(path, X_OK) == 0) setenv("RENDER_SERVER_EXEC_PATH", path, 0);
        flags |= VIRGL_RENDERER_NEPTUNE | VIRGL_RENDERER_RENDER_SERVER;
#define VO(n) *(void **)&vr.n = dlsym(r, "virgl_renderer_" #n);
        VO(context_create_with_flags) VO(resource_create_blob) VO(resource_map) VO(resource_unmap) VO(resource_map_fixed)
        VO(resource_get_map_info) VO(context_create_fence) VO(create_handle_for_scanout) VO(release_handle_for_scanout) VO(reset)
#undef VO
        if (!vr.context_create_with_flags || !vr.resource_create_blob || !vr.resource_map || !vr.context_create_fence) {
            fprintf(stderr, "hengevm: virgl: this virglrenderer has no blob resources\n");
            return -1;
        }
        v->present = 0; /* the picture is read into the device's own display buffer */
    }
    int rc = vr.init(v, flags, &callbacks);
    if (rc) { fprintf(stderr, "hengevm: virgl: virgl_renderer_init failed (%d)\n", rc); return -1; }
    fprintf(stderr, "hengevm: virgl: up, EGL %d.%d (ANGLE)\n", major, minor);
    if (v->g->d3d) {
        static const uint32_t ids[] = {1, 2, CAPSET_NEPTUNE};
        for (unsigned i = 0; i < 3; i++) {
            uint32_t ver = 0, size = 0;
            vr.get_cap_set(ids[i], &ver, &size);
            if (size) v->capset[v->ncapsets++] = ids[i];
            if (ids[i] == CAPSET_NEPTUNE) fprintf(stderr, "hengevm: virgl: Direct3D (Neptune) %s\n", size ? "is there" : "is NOT there: no backend library?");
        }
    }
    return 0;
}

/* A blob is no longer what it was as a scanout: its texture and surface go. */
static void scan_forget(HvGpuVirgl *v, Res *r) {
    pthread_mutex_lock(&v->present_mu);
    if (v->scan_surf == r->surf) v->scan_surf = NULL;
    pthread_mutex_unlock(&v->present_mu);
    if (r->surf) CFRelease(r->surf);
    if (r->tex && vr.release_handle_for_scanout) vr.release_handle_for_scanout(VIRGL_NATIVE_HANDLE_METAL_TEXTURE, r->tex);
    r->surf = NULL, r->tex = NULL;
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
            hostmem_unmap(v, r);
            scan_forget(v, r);
            vr.resource_unref(id);
            if (v->scanout_res == id) v->scanout_res = 0;
            free(r);
            return;
        }
}

/* Multisampling, pretended. virglrenderer keeps a multisampled surface in a multisample texture, which ANGLE's
   OpenGL ES 3.0 does not have, so it says one sample and Mesa in the guest gives GL_MAX_SAMPLES 1. ES 3.0 asks for
   4: Chromium's own ANGLE finds less and refuses ES 3 on the device (its GPU process exits, and pages are drawn by
   the CPU). So the capabilities say 4 samples, for every format that can be rendered to, and a resource asked for
   with samples is made without them (create): what is drawn into it comes out right, just not antialiased.
   HV_VIRGL_MSAA=0: as virglrenderer says. Offsets: struct virgl_caps_v1/v2 in virglrenderer's virgl_hw.h. */
enum { CAPS_RENDER = 68, CAPS_DEPTHSTENCIL = 132, CAPS_BSET = 260, CAPS_MAX_SAMPLES = 284, CAPS_V1_SIZE = 308,
       CAPS_MULTISAMPLE_FORMATS = 768, CAPS_MASK_WORDS = 16, CAPS_BSET_TEXTURE_MULTISAMPLE = 1u << 14 };
static int msaa_pretended;

static void caps_pretend_msaa(uint8_t *caps, uint32_t size) {
    static int on = -1;
    if (on < 0) on = !(getenv("HV_VIRGL_MSAA") && !atoi(getenv("HV_VIRGL_MSAA")));
    uint32_t samples, bset;
    if (!on || size < CAPS_V1_SIZE) return;
    memcpy(&samples, caps + CAPS_MAX_SAMPLES, 4);
    if (samples > 1 && !msaa_pretended) return; /* a host that has it */
    msaa_pretended = 1;
    samples = 4;
    memcpy(caps + CAPS_MAX_SAMPLES, &samples, 4);
    memcpy(&bset, caps + CAPS_BSET, 4);
    bset |= CAPS_BSET_TEXTURE_MULTISAMPLE;
    memcpy(caps + CAPS_BSET, &bset, 4);
    if (size < CAPS_MULTISAMPLE_FORMATS + CAPS_MASK_WORDS * 4) return;
    for (int i = 0; i < CAPS_MASK_WORDS; i++) {
        uint32_t r, d;
        memcpy(&r, caps + CAPS_RENDER + i * 4, 4);
        memcpy(&d, caps + CAPS_DEPTHSTENCIL + i * 4, 4);
        r |= d;
        memcpy(caps + CAPS_MULTISAMPLE_FORMATS + i * 4, &r, 4);
    }
}

static uint32_t create(HvGpuVirgl *v, struct virgl_renderer_resource_create_args *a) {
    if (!a->handle || find(v, a->handle)) return RESP_ERR_INVALID_RESOURCE_ID;
    Res *r = calloc(1, sizeof *r);
    if (!r) return RESP_ERR_OUT_OF_MEMORY;
    /* Henge reads the scanout back rather than sharing it with the host, so nothing is shared: without SCANOUT and
       SHARED a resource is an ordinary GL texture. With them virglrenderer backs it by an external image, and stores
       an alpha-less one (B8G8R8X8, which Linux scans out) as RGB8, which ANGLE can neither render to nor read back. */
    if (!v->g->d3d) a->bind &= ~(VIRGL_BIND_SCANOUT | VIRGL_BIND_SHARED); /* a Direct3D device's are as its driver asks */
    if (msaa_pretended && a->nr_samples > 1) a->nr_samples = 0; /* see caps_pretend_msaa */
    if (vr.resource_create(a, NULL, 0)) { free(r); return RESP_ERR_UNSPEC; }
    r->id = a->handle, r->format = a->format, r->w = a->width, r->h = a->height;
    r->next = v->res;
    v->res = r;
    return RESP_OK_NODATA;
}

static void present_drop(HvGpuVirgl *v) {
    pthread_mutex_lock(&v->present_mu);
    v->front = -1;
    for (int i = 0; i < 2; i++) {
        if (v->surf_fbo[i]) gl.DeleteFramebuffers(1, &v->surf_fbo[i]);
        if (v->pbuf[i]) egl.ReleaseTexImage(v->dpy, v->pbuf[i], EGL_BACK_BUFFER);
        if (v->surf_tex[i]) gl.DeleteTextures(1, &v->surf_tex[i]);
        if (v->pbuf[i]) egl.DestroySurface(v->dpy, v->pbuf[i]);
        if (v->surf[i]) CFRelease(v->surf[i]);
        v->surf_fbo[i] = v->surf_tex[i] = 0, v->pbuf[i] = NULL, v->surf[i] = NULL;
    }
    if (v->read_fbo) gl.DeleteFramebuffers(1, &v->read_fbo);
    v->read_fbo = 0, v->surf_w = v->surf_h = 0;
    pthread_mutex_unlock(&v->present_mu);
}

static CFNumberRef num(int32_t x) { return CFNumberCreate(NULL, kCFNumberSInt32Type, &x); }

/* Two BGRA IOSurfaces of the scanout's size, each bound to a texture in a framebuffer. In context 0, current. */
static int present_make(HvGpuVirgl *v, uint32_t w, uint32_t h) {
    present_drop(v);
    EGLint target = EGL_TEXTURE_2D;
    if (v->present == 1) egl.GetConfigAttrib(v->dpy, v->cfg, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &target);
    if (target != EGL_TEXTURE_2D) return -1; /* a rectangle texture (ANGLE on desktop OpenGL): not handled */
    pthread_mutex_lock(&v->present_mu);
    int ok = 1;
    for (int i = 0; i < 2 && ok; i++) {
        const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
        CFNumberRef vals[] = {num((int32_t)w), num((int32_t)h), num(4), num((int32_t)0x42475241 /* 'BGRA' */)};
        CFDictionaryRef d = CFDictionaryCreate(NULL, keys, (const void **)vals, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        v->surf[i] = IOSurfaceCreate(d);
        CFRelease(d);
        for (int k = 0; k < 4; k++) CFRelease(vals[k]);
        const EGLint a[] = {EGL_WIDTH, (EGLint)w, EGL_HEIGHT, (EGLint)h, EGL_IOSURFACE_PLANE_ANGLE, 0, EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
                            EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_BGRA_EXT, EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
                            EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE, EGL_NONE};
        if (!v->surf[i]) { ok = 0; break; }
        if (v->present == 2) continue; /* written through its memory: no texture */
        v->pbuf[i] = egl.CreatePbufferFromClientBuffer(v->dpy, EGL_IOSURFACE_ANGLE, (void *)v->surf[i], v->cfg, a);
        if (!v->pbuf[i]) { ok = 0; break; }
        gl.GenTextures(1, &v->surf_tex[i]);
        gl.BindTexture(GL_TEXTURE_2D, v->surf_tex[i]);
        ok = egl.BindTexImage(v->dpy, v->pbuf[i], EGL_BACK_BUFFER) != 0;
        gl.GenFramebuffers(1, &v->surf_fbo[i]);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, v->surf_fbo[i]);
        gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->surf_tex[i], 0);
        if (ok) ok = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (ok) gl.GenFramebuffers(1, &v->read_fbo), v->surf_w = w, v->surf_h = h, v->back = 0;
    pthread_mutex_unlock(&v->present_mu);
    if (!ok) present_drop(v);
    return ok ? 0 : -1;
}

/* The whole scanout into the back surface, on the GPU; then that one is the front. 0 if done. */
static int present_flush(HvGpuVirgl *v, Res *r) {
    struct virgl_renderer_resource_info info = {0};
    if (vr.resource_get_info((int)r->id, &info) || !info.tex_id) return -1;
    vr.force_ctx_0(); /* virglrenderer's own context, and it knows it is current again */
    int read0 = 0, draw0 = 0, tex0 = 0;
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read0);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    int scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    int rc = -1;
    if ((v->surf_w == r->w && v->surf_h == r->h) || present_make(v, r->w, r->h) == 0) {
        int b = v->back;
        if (scissor) gl.Disable(GL_SCISSOR_TEST); /* it clips a blit too */
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, v->read_fbo);
        gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, info.tex_id, 0);
        if (v->present == 2) {
            /* Read into the surface's own memory: one copy, and the layer shows it. (On an iPad the GPU blit below
               stopped reaching the surface after a while: ANGLE's texture and the IOSurface's memory came apart.) */
            int flip2 = (info.flags & RESOURCE_FLAG_Y_0_TOP) != 0;
            IOSurfaceRef sf = v->surf[b];
            IOSurfaceLock(sf, 0, NULL);
            uint8_t *base = IOSurfaceGetBaseAddress(sf);
            size_t stride = IOSurfaceGetBytesPerRow(sf);
            unsigned err2;
            if (stride == (size_t)r->w * 4 && !flip2) {
                gl.ReadPixels(0, 0, (int)r->w, (int)r->h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, base);
                err2 = gl.GetError();
            } else {
                size_t px = (size_t)r->w * r->h;
                if (px > v->readback_px) {
                    free(v->readback);
                    v->readback = malloc(px * 4);
                    v->readback_px = v->readback ? px : 0;
                }
                err2 = 1;
                if (v->readback) {
                    gl.ReadPixels(0, 0, (int)r->w, (int)r->h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, v->readback);
                    err2 = gl.GetError();
                    for (uint32_t y = 0; y < r->h && !err2; y++)
                        memcpy(base + (size_t)y * stride, v->readback + (size_t)(flip2 ? r->h - 1 - y : y) * r->w, (size_t)r->w * 4);
                }
            }
            IOSurfaceUnlock(sf, 0, NULL);
            gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            if (!err2) {
                pthread_mutex_lock(&v->present_mu);
                v->front = b, v->back = !b;
                pthread_mutex_unlock(&v->present_mu);
                rc = 0;
            }
            goto restore;
        }
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, v->surf_fbo[b]);
        /* The IOSurface's first row is the picture's top, and so is the first row of the texture a guest's GPU
           driver scans out (seen: Hyprland on virgl); a Y_0_TOP resource (a 2D one) is kept the other way up. */
        int flip = (info.flags & RESOURCE_FLAG_Y_0_TOP) != 0;
        gl.BlitFramebuffer(0, flip ? (int)r->h : 0, (int)r->w, flip ? 0 : (int)r->h, 0, 0, (int)r->w, (int)r->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        unsigned err = gl.GetError();
        static unsigned looked;
        if (getenv("HV_GPU_LOG") && (looked < 4 || (looked & 255) == 0)) { /* is the picture in the source, and in the surface? */
            static uint32_t px[64 * 64];
            uint64_t src = 0, dst = 0;
            unsigned st = gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            memset(px, 0, sizeof px);
            gl.ReadPixels(0, flip ? (int)r->h - 64 : 0, 64, 64, 0x1908, GL_UNSIGNED_BYTE, px);
            unsigned e1 = gl.GetError();
            for (int i = 0; i < 64 * 64; i++) src += px[i] & 0xffffff;
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, v->surf_fbo[b]);
            memset(px, 0, sizeof px);
            gl.ReadPixels(0, 0, 64, 64, 0x1908, GL_UNSIGNED_BYTE, px);
            unsigned e2 = gl.GetError();
            for (int i = 0; i < 64 * 64; i++) dst += px[i] & 0xffffff;
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, v->read_fbo);
            gl.Finish();
            uint64_t mem = 0;
            IOSurfaceLock(v->surf[b], kIOSurfaceLockReadOnly, NULL);
            const uint8_t *base = IOSurfaceGetBaseAddress(v->surf[b]);
            size_t stride = IOSurfaceGetBytesPerRow(v->surf[b]);
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint32_t p4; memcpy(&p4, base + y * stride + x * 4, 4); mem += p4 & 0xffffff; }
            IOSurfaceUnlock(v->surf[b], kIOSurfaceLockReadOnly, NULL);
            fprintf(stderr, "hengevm: virgl: present %u: %ux%u res %u tex %u fmt %u flags %#x: blit error %#x, source fbo %#x sum %llx (%#x), surface sum %llx (%#x), in its memory %llx (stride %zu, %zux%zu, in use %d)\n",
                    looked, r->w, r->h, r->id, info.tex_id, info.virgl_format, info.flags, err, st, (unsigned long long)src, e1, (unsigned long long)dst, e2,
                    (unsigned long long)mem, stride, IOSurfaceGetWidth(v->surf[b]), IOSurfaceGetHeight(v->surf[b]), (int)IOSurfaceIsInUse(v->surf[b]));
        }
        looked++;
        gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        if (!err) {
            gl.Flush();
            pthread_mutex_lock(&v->present_mu);
            v->front = b, v->back = !b;
            pthread_mutex_unlock(&v->present_mu);
            rc = 0;
        }
    restore:
        if (scissor) gl.Enable(GL_SCISSOR_TEST);
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)read0);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)draw0);
    gl.BindTexture(GL_TEXTURE_2D, (unsigned)tex0);
    return rc;
}

IOSurfaceRef hv_gpu_virgl_surface(HvGpuVirgl *v) {
    if (!v || (!v->present && !v->g->d3d)) return NULL;
    pthread_mutex_lock(&v->present_mu);
    IOSurfaceRef s = v->g->d3d ? v->scan_surf : v->front >= 0 ? v->surf[v->front] : NULL;
    if (s) CFRetain(s);
    pthread_mutex_unlock(&v->present_mu);
    return s;
}

/* The newest presented frame, copied out (a screenshot, a host without a layer to show the surface in). 1 if
   there was one. */
int hv_gpu_virgl_read(HvGpuVirgl *v, uint32_t *out, uint32_t w, uint32_t h) {
    IOSurfaceRef s = hv_gpu_virgl_surface(v);
    if (!s) return 0;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *base = IOSurfaceGetBaseAddress(s);
    size_t stride = IOSurfaceGetBytesPerRow(s), sw = IOSurfaceGetWidth(s), sh = IOSurfaceGetHeight(s);
    memset(out, 0, (size_t)w * h * 4);
    for (uint32_t y = 0; y < h && y < sh; y++) memcpy(out + (size_t)y * w, base + y * stride, (w < sw ? w : sw) * 4);
    if (IOSurfaceGetPixelFormat(s) == 'RGBA')
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) out[i] = (out[i] & 0xff00ff00u) | (out[i] >> 16 & 0xff) | (out[i] & 0xff) << 16;
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    CFRelease(s);
    return 1;
}

/* ---- a Direct3D device: blobs in the shared memory region, the display's EDID, a blob as the scanout ---- */

/* A host blob into the guest: the pages of the shared region from off are the blob's memory. The region is a
   paged RAM region of the machine at the BAR's address (added the first time, when the driver has placed the
   BAR): the guest's driver places blobs at any multiple of its 4 KB pages, the host's pages are 16 KB, and so
   each 4 KB page is given the place in virglrenderer's own mapping of the blob that it stands for. */
static uint32_t hostmem_map(HvGpuVirgl *v, Res *r, uint64_t off, uint32_t *map_info) {
    HvVirtioGpu *g = v->g;
    uint64_t len = (r->blob_size + 0xfff) & ~0xfffull;
    if (!r->blob || r->mapped || !len || (off & 0xfff) || off > g->hostmem_size || len > g->hostmem_size - off) {
        fprintf(stderr, "hengevm: virtio-gpu: blob %u (%llu bytes) cannot be mapped at %#llx of the shared region\n", r->id,
                (unsigned long long)r->blob_size, (unsigned long long)off);
        return RESP_ERR_INVALID_PARAMETER;
    }
    uint64_t bar = hv_pci_bar_addr(&g->vio.pci, 2);
    if (!g->hostmem_ram) {
        if (!bar || !(g->hostmem_ram = hv_machine_add_paged_ram(g->m, bar, g->hostmem_size))) return RESP_ERR_UNSPEC;
        g->hostmem_pa = bar;
        if (g->m->log) fprintf(stderr, "hengevm: virtio-gpu: the shared region (%llu MB) is at %#llx\n",
                               (unsigned long long)(g->hostmem_size >> 20), (unsigned long long)bar);
    } else if (bar != g->hostmem_pa) {
        fprintf(stderr, "hengevm: virtio-gpu: the shared region was moved from %#llx to %#llx: not followed\n",
                (unsigned long long)g->hostmem_pa, (unsigned long long)bar);
        return RESP_ERR_UNSPEC;
    }
    void *data = NULL;
    uint64_t size = 0;
    if (vr.resource_map(r->id, &data, &size) || !data) return RESP_ERR_UNSPEC;
    if (size < len) len = size & ~0xfffull;
    hv_ram_set_pages(g->m, g->hostmem_ram, off >> 12, len >> 12, data);
    r->mapped = 1, r->map_off = off, r->map_len = len;
    *map_info = 0;
    if (vr.resource_get_map_info) vr.resource_get_map_info(r->id, map_info);
    return RESP_OK_MAP_INFO;
}

static void hostmem_unmap(HvGpuVirgl *v, Res *r) {
    if (!r->mapped) return;
    hv_ram_set_pages(v->g->m, v->g->hostmem_ram, r->map_off >> 12, r->map_len >> 12, NULL); /* and no CPU has them in its TLB */
    vr.resource_unmap(r->id);
    r->mapped = 0;
}

/* The display's EDID: one detailed timing, the size the host would like, at 60 Hz. */
static void edid_make(uint8_t e[128], uint32_t w, uint32_t h) {
    static const uint8_t head[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x21, 0xc7 /* "HNG" */, 0x34, 0x12, 0, 0, 0, 0, 1, 36, 1, 4,
                                   0xa5, 0, 0, 0x78, 0x06, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, 0, 0, 0};
    memset(e, 0, 128);
    memcpy(e, head, sizeof head);
    uint32_t mm_w = w * 254 / 960, mm_h = h * 254 / 960; /* 96 pixels an inch */
    e[21] = (uint8_t)(mm_w / 10), e[22] = (uint8_t)(mm_h / 10);
    for (int i = 38; i < 54; i++) e[i] = 0x01; /* no standard timings */
    uint32_t hbl = 160, vbl = 45, hfp = 48, hs = 32, vfp = 3, vs = 5;
    uint32_t clk = (w + hbl) * (h + vbl) * 60 / 10000;
    uint8_t *d = e + 54;
    d[0] = (uint8_t)clk, d[1] = (uint8_t)(clk >> 8);
    d[2] = (uint8_t)w, d[3] = (uint8_t)hbl, d[4] = (uint8_t)((w >> 8) << 4 | hbl >> 8);
    d[5] = (uint8_t)h, d[6] = (uint8_t)vbl, d[7] = (uint8_t)((h >> 8) << 4 | vbl >> 8);
    d[8] = (uint8_t)hfp, d[9] = (uint8_t)hs, d[10] = (uint8_t)(vfp << 4 | vs), d[11] = 0;
    d[12] = (uint8_t)mm_w, d[13] = (uint8_t)mm_h, d[14] = (uint8_t)((mm_w >> 8) << 4 | mm_h >> 8);
    d[17] = 0x1e; /* digital, separate syncs, both positive */
    static const uint8_t name[18] = {0, 0, 0, 0xfc, 0, 'H', 'e', 'n', 'g', 'e', '\n', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    memcpy(e + 72, name, 18);
    static const uint8_t range[18] = {0, 0, 0, 0xfd, 0, 30, 120, 30, 200, 60, 0, 0x0a, ' ', ' ', ' ', ' ', ' ', ' '};
    memcpy(e + 90, range, 18);
    e[111] = 0x10; /* an unused descriptor */
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) sum = (uint8_t)(sum + e[i]);
    e[127] = (uint8_t)(0 - sum);
}

/* The picture's size is the guest's choice on a Direct3D device. */
static void display_size(HvGpuVirgl *v, uint32_t w, uint32_t h) {
    HvVirtioGpu *g = v->g;
    if (!g->d3d || !w || !h) return;
    w = w > GPU_MAX_W ? GPU_MAX_W : w, h = h > GPU_MAX_H ? GPU_MAX_H : h;
    if (w == g->width && h == g->height) return;
    hv_dev_lock(g->m); /* what a reader of the display takes (host/vm.c) */
    g->width = w, g->height = h;
    hv_dev_unlock(g->m);
    if (g->m->log) fprintf(stderr, "hengevm: virtio-gpu: the guest's picture is %ux%u\n", w, h);
}

/* A blob as the scanout, flushed: virglrenderer's texture of it (a host blob the guest's Direct3D drew) read
   into the display, or the guest's own memory (a blob of its RAM) copied. */
static uint32_t flush_blob(HvGpuVirgl *v, Res *r) {
    HvVirtioGpu *g = v->g;
    uint32_t w = 0, h = 0;
    if (r->surf) { /* shown as it is, or copied by whoever wants the pixels (hv_gpu_virgl_read) */
    } else if (r->tex) {
        if (hv_metal_texture_read(r->tex, g->display, g->width, g->height, &w, &h) != 0) return RESP_ERR_UNSPEC;
        if (w != g->width) { /* narrower than the display: its rows go where the display's stride has them */
            for (uint32_t y = h; y-- > 0;) memmove(g->display + (size_t)y * g->width, g->display + (size_t)y * w, (size_t)w * 4);
        }
    } else if (r->iov) {
        uint32_t rows = r->sh < g->height ? r->sh : g->height, cols = r->sw < g->width ? r->sw : g->width;
        for (uint32_t y = 0; y < rows; y++) {
            uint64_t at = r->soffset + (uint64_t)y * r->sstride, left = (uint64_t)cols * 4;
            uint8_t *dst = (uint8_t *)(g->display + (size_t)y * g->width);
            for (int i = 0; i < r->niov && left; i++) {
                if (at >= r->iov[i].iov_len) { at -= r->iov[i].iov_len; continue; }
                uint64_t n = r->iov[i].iov_len - at < left ? r->iov[i].iov_len - at : left;
                memcpy(dst, (uint8_t *)r->iov[i].iov_base + at, n);
                dst += n, left -= n, at = 0;
            }
        }
        if (r->sformat != B8G8R8A8 && r->sformat != B8G8R8X8)
            for (uint32_t y = 0; y < rows; y++)
                for (uint32_t x = 0; x < cols; x++) g->display[(size_t)y * g->width + x] = to_display(r->sformat, g->display[(size_t)y * g->width + x]);
    } else return RESP_ERR_UNSPEC;
    g->flushes++;
    g->changes++;
    return RESP_OK_NODATA;
}

/* The scanout's resource, the flushed rectangle of it, onto the display. */
static uint32_t flush(HvGpuVirgl *v, const uint8_t *req) {
    HvVirtioGpu *g = v->g;
    GpuRect rc;
    uint32_t id;
    memcpy(&rc, req + 24, 16);
    memcpy(&id, req + 40, 4);
    Res *r = find(v, id);
    if (!r) return RESP_ERR_INVALID_RESOURCE_ID;
    if (id != v->scanout_res) return RESP_OK_NODATA;
    if (r->blob) return flush_blob(v, r);
    if (v->present) {
        if (present_flush(v, r) == 0) {
            g->flushes++;
            g->changes++;
            return RESP_OK_NODATA;
        }
        static int said;
        if (!said++) fprintf(stderr, "hengevm: virgl: cannot present through an IOSurface; reading frames back instead\n");
        present_drop(v);
        v->present = 0;
    }
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
        uint32_t mode[6] = {0, 0, g->d3d ? g->want_w : g->width, g->d3d ? g->want_h : g->height, 1, 0};
        memcpy(resp + 24, mode, sizeof mode);
        resp_len = 24 + MAX_SCANOUTS * 24;
        break;
    }
    case CMD_GET_CAPSET_INFO: {
        uint32_t index;
        memcpy(&index, req + 24, 4);
        uint32_t info[4] = {0};
        if (g->d3d ? index < v->ncapsets : index < 2) {
            info[0] = g->d3d ? v->capset[index] : index + 1; /* VIRGL, VIRGL2 (and Neptune) */
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
        if (!g->d3d) caps_pretend_msaa(out + 24, max_size);
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
        if (id) {
            GpuRect sr;
            memcpy(&sr, req + 24, 16);
            Res *r = find(v, id);
            display_size(v, sr.w ? sr.w : r->w, sr.h ? sr.h : r->h);
        }
        pthread_mutex_lock(&v->present_mu);
        v->scan_surf = NULL; /* not a blob's surface */
        pthread_mutex_unlock(&v->present_mu);
        v->scanout_res = id;
        g->scanout_res = id;
        g->enabled = id != 0;
        if (!id) memset(g->display, 0, (size_t)g->width * g->height * 4);
        break;
    }
    case CMD_GET_EDID: {
        uint32_t size = 128;
        memcpy(resp + 24, &size, 4);
        static uint8_t edid[1024];
        edid_make(edid, g->d3d ? g->want_w : g->width, g->d3d ? g->want_h : g->height);
        Hdr o = {.type = RESP_OK_EDID, .flags = h.flags & FLAG_FENCE, .fence_id = h.fence_id, .ctx_id = h.ctx_id};
        memcpy(resp, &o, sizeof o);
        uint32_t wr = hv_virtq_write(&g->vio, e, 0, resp, 32);
        wr += hv_virtq_write(&g->vio, e, 32, edid, sizeof edid);
        return wr;
    }
    case CMD_RESOURCE_CREATE_BLOB: {
        uint32_t c[4]; /* resource_id, blob_mem, blob_flags, nr_entries */
        uint64_t blob_id, size;
        memcpy(c, req + 24, 16);
        memcpy(&blob_id, req + 40, 8);
        memcpy(&size, req + 48, 8);
        if (!g->d3d) { type = RESP_ERR_INVALID_PARAMETER; break; }
        if (!c[0] || find(v, c[0])) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        Res *r = calloc(1, sizeof *r);
        if (!r) { type = RESP_ERR_OUT_OF_MEMORY; break; }
        r->id = c[0], r->blob = 1, r->blob_mem = c[1], r->blob_size = size;
        if (c[1] != BLOB_MEM_HOST3D && c[3]) { /* its memory is the guest's: the entries say where */
            if (c[3] > 1u << 20) { free(r); type = RESP_ERR_INVALID_PARAMETER; break; }
            r->iov = calloc(c[3], sizeof *r->iov);
            r->ents = calloc(c[3], sizeof *r->ents);
            for (uint32_t i = 0; r->iov && r->ents && i < c[3]; i++) {
                uint8_t ent[16];
                uint64_t addr;
                uint32_t len;
                if (hv_virtq_read(&g->vio, e, 56 + i * 16, ent, 16) != 16) { type = RESP_ERR_INVALID_PARAMETER; break; }
                memcpy(&addr, ent, 8);
                memcpy(&len, ent + 8, 4);
                void *p = hv_phys_ptr(g->m, addr, len);
                if (!p) { type = RESP_ERR_INVALID_PARAMETER; break; }
                r->iov[i] = (struct iovec){p, len};
                r->ents[i].addr = addr, r->ents[i].len = len;
            }
            if (!r->iov || !r->ents) type = RESP_ERR_OUT_OF_MEMORY;
            r->niov = (int)c[3];
        }
        struct virgl_renderer_resource_create_blob_args a = {
            .res_handle = c[0], .ctx_id = h.ctx_id, .blob_mem = c[1], .blob_flags = c[2], .blob_id = blob_id, .size = size,
            .iovecs = r->iov, .num_iovs = (uint32_t)r->niov};
        int brc = type == RESP_OK_NODATA ? vr.resource_create_blob(&a) : -1;
        if (getenv("HV_GPU_LOG"))
            fprintf(stderr, "gpu3d: blob %u ctx %u mem %u flags %#x id %llu size %llu entries %u -> %d\n", c[0], h.ctx_id, c[1], c[2],
                    (unsigned long long)blob_id, (unsigned long long)size, c[3], brc);
        if (brc) {
            free(r->iov), free(r->ents), free(r);
            if (type == RESP_OK_NODATA) type = RESP_ERR_UNSPEC;
            break;
        }
        r->next = v->res;
        v->res = r;
        break;
    }
    case CMD_RESOURCE_MAP_BLOB: {
        uint32_t id, info = 0;
        uint64_t off;
        memcpy(&id, req + 24, 4);
        memcpy(&off, req + 32, 8);
        Res *r = find(v, id);
        if (!r) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        type = hostmem_map(v, r, off, &info);
        if (getenv("HV_GPU_LOG")) fprintf(stderr, "gpu3d: map blob %u at %#llx -> %#x (how %d, info %#x)\n", id, (unsigned long long)off, type, r->mapped, info);
        if (type == RESP_OK_MAP_INFO) memcpy(resp + 24, &info, 4), resp_len = 32;
        break;
    }
    case CMD_RESOURCE_UNMAP_BLOB: {
        uint32_t id;
        memcpy(&id, req + 24, 4);
        Res *r = find(v, id);
        if (!r) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        hostmem_unmap(v, r);
        break;
    }
    case CMD_SET_SCANOUT_BLOB: {
        GpuRect sr;
        uint32_t c[6], stride, offset; /* scanout_id, resource_id, width, height, format, padding */
        memcpy(&sr, req + 24, 16);
        memcpy(c, req + 40, 24);
        memcpy(&stride, req + 64, 4);
        memcpy(&offset, req + 80, 4);
        if (c[0] != 0) { type = RESP_ERR_INVALID_SCANOUT_ID; break; }
        if (!c[1]) {
            v->scanout_res = g->scanout_res = 0;
            g->enabled = 0;
            pthread_mutex_lock(&v->present_mu);
            v->scan_surf = NULL;
            pthread_mutex_unlock(&v->present_mu);
            break;
        }
        Res *r = find(v, c[1]);
        if (!r || !r->blob) { type = RESP_ERR_INVALID_RESOURCE_ID; break; }
        if (c[2] < 16 || c[3] < 16 || sr.w < 16 || sr.h < 16 || sr.x + sr.w > c[2] || sr.y + sr.h > c[3]) { type = RESP_ERR_INVALID_PARAMETER; break; }
        /* Windows flips between a few blobs, one SET_SCANOUT_BLOB a frame: a blob keeps its texture while its
           layout as a scanout stays what it was. */
        uint32_t st = stride ? stride : c[2] * 4;
        if (!r->tex || r->sw != c[2] || r->sh != c[3] || r->sformat != c[4] || r->sstride != st || r->soffset != offset) {
            scan_forget(v, r);
            void *handle = NULL;
            int kind = vr.create_handle_for_scanout ? vr.create_handle_for_scanout(c[1], c[2], c[3], c[4], c[5], stride, offset, &handle) : 0;
            if (kind == VIRGL_NATIVE_HANDLE_METAL_TEXTURE) r->tex = handle, r->surf = hv_metal_texture_surface(handle);
            else if (kind && vr.release_handle_for_scanout) vr.release_handle_for_scanout(kind, handle);
            if (g->m->log || getenv("HV_GPU_LOG"))
                fprintf(stderr, "hengevm: virtio-gpu: scanout blob %u, %ux%u format %u stride %u offset %u: %s\n", c[1], c[2], c[3], c[4],
                        stride, offset, r->surf ? "a Metal texture on an IOSurface" : r->tex ? "a Metal texture" : r->iov ? "the guest's memory" : "NOTHING to show it from");
        }
        pthread_mutex_lock(&v->present_mu);
        v->scan_surf = r->surf;
        pthread_mutex_unlock(&v->present_mu);
        if (!r->tex && !r->iov) { type = RESP_ERR_UNSPEC; break; }
        r->sw = c[2], r->sh = c[3], r->sformat = c[4], r->sstride = stride ? stride : c[2] * 4, r->soffset = offset;
        display_size(v, sr.w, sr.h);
        v->scanout_res = g->scanout_res = c[1];
        g->enabled = 1;
        break;
    }
    case CMD_RESOURCE_FLUSH: type = flush(v, req); break;
    case CMD_TRANSFER_TO_HOST_2D: {
        GpuRect r;
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
        uint32_t init;
        memcpy(&init, req + 28, 4); /* context_init: the capability set (protocol) the context speaks, or 0 */
        int crc = init && vr.context_create_with_flags ? vr.context_create_with_flags(h.ctx_id, init, nlen, name)
                                                       : vr.context_create(h.ctx_id, nlen, name);
        if (g->d3d && (g->m->log || getenv("HV_GPU_LOG")))
            fprintf(stderr, "hengevm: virtio-gpu: context %u \"%s\" (protocol %u) -> %d\n", h.ctx_id, name, init & 0xff, crc);
        if (crc) type = RESP_ERR_UNSPEC;
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
    if (h.flags & FLAG_RING_IDX) out.flags |= FLAG_RING_IDX;
    memcpy(resp, &out, sizeof out);
    if (h.flags & FLAG_FENCE) {
        q->fenced = 1, q->fence = (uint32_t)h.fence_id;
        q->ringed = (h.flags & FLAG_RING_IDX) != 0, q->ctx = h.ctx_id, q->ring = h.ring_idx, q->fence64 = h.fence_id;
    }
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
    g->cursor_gen++;
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

/* The driver reset the device: every resource and context goes (the worker's, as they were made). */
static void let_go(HvGpuVirgl *v) {
    v->reset_req = 0;
    v->scanout_res = 0;
    while (v->res) unref(v, v->res->id);
    if (vr.reset) vr.reset();
    if (v->g->m->log) fprintf(stderr, "hengevm: virtio-gpu: reset by its driver\n");
}

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
        if (v->stop) {
            pthread_mutex_unlock(&v->mu);
            break;
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
            if (v->reset_req && v->taken == v->reset_at) let_go(v);
            Req *q = &v->ring[v->taken % RING];
            q->fenced = q->ringed = 0;
            if (q->q == CONTROLQ) q->written = control(v, q);
            else cursor(v, &q->e), q->written = 0;
            v->taken++; /* before its fence can pass: the callback looks up to here */
            if (q->fenced && q->ringed) {
                if (vr.context_create_fence(q->ctx, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE, q->ring, q->fence64))
                    __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE), finished = 1; /* no such context: nothing to wait for */
            } else if (q->fenced) vr.create_fence((int)q->fence, 0);
            else __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE), finished = 1;
        }
        if (v->reset_req && v->taken == v->reset_at) let_go(v);
        vr.poll();
        for (uint32_t i = v->published; i != v->taken; i++) {
            Req *q = &v->ring[i % RING];
            if (q->fenced && !q->ringed && !q->done && fence_passed(v, q->fence)) __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE), finished = 1;
        }
        if (finished) hv_machine_wake(v->g->m);
    }
    /* Stopped (the machine is being released): everything this thread made, so another machine can start 3D. */
    if (vr.force_ctx_0) vr.force_ctx_0();
    present_drop(v);
    while (v->res) unref(v, v->res->id);
    if (vr.cleanup) vr.cleanup(v);
    egl.MakeCurrent(v->dpy, NULL, NULL, NULL);
    if (v->main_ctx) egl.DestroyContext(v->dpy, v->main_ctx);
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
        q->epoch = v->epoch;
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
        q->pushed = 1;
        if (q->epoch != v->epoch) continue; /* queued before a reset: its queue is gone */
        hv_virtq_push(vio, q->q, &q->e, q->written);
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
    pthread_mutex_init(&v->present_mu, NULL);
    v->front = -1;
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

uint32_t hv_gpu_virgl_capsets(HvGpuVirgl *v) { return v ? v->ncapsets : 0; }

void hv_gpu_virgl_reset(HvGpuVirgl *v) {
    if (!v) return;
    pthread_mutex_lock(&v->mu);
    v->epoch++;
    v->reset_at = v->submitted;
    v->reset_req = 1;
    pthread_mutex_unlock(&v->mu);
    pthread_cond_signal(&v->cv);
}

void hv_gpu_virgl_stop(HvGpuVirgl *v) {
    if (!v) return;
    pthread_mutex_lock(&v->mu);
    v->stop = 1;
    pthread_cond_signal(&v->cv);
    pthread_mutex_unlock(&v->mu);
    pthread_join(v->thread, NULL);
    pthread_mutex_destroy(&v->mu);
    pthread_mutex_destroy(&v->present_mu);
    pthread_cond_destroy(&v->cv);
    free(v->ring);
    free(v->readback);
    free(v);
}
