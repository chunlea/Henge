// SPDX-License-Identifier: GPL-2.0-or-later
/* A Metal texture's pixels, for virtio_gpu_virgl.c: the picture a guest's Direct3D driver scans out is
   handed over by virglrenderer as an MTLTexture (virgl_renderer_create_handle_for_scanout). */
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>

#include <stdint.h>
#include <string.h>

/* The texture's top-left max_w x max_h pixels (or all of it) into out, B8G8R8X8, stride *w. 0, or -1. */
int hv_metal_texture_read(void *texture, uint32_t *out, uint32_t max_w, uint32_t max_h, uint32_t *w, uint32_t *h) {
    @autoreleasepool {
        id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
        if (!t) return -1;
        uint32_t tw = (uint32_t)t.width, th = (uint32_t)t.height;
        if (tw > max_w) tw = max_w;
        if (th > max_h) th = max_h;
        if (!tw || !th) return -1;
        int rgba = t.pixelFormat == MTLPixelFormatRGBA8Unorm || t.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
        IOSurfaceRef s = t.iosurface;
        if (s) { /* shared with the process that drew it: read its memory */
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *base = IOSurfaceGetBaseAddress(s);
            size_t stride = IOSurfaceGetBytesPerRow(s);
            for (uint32_t y = 0; y < th; y++) memcpy(out + (size_t)y * tw, base + y * stride, (size_t)tw * 4);
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
        } else {
            id<MTLTexture> from = t;
            if (t.storageMode == MTLStorageModePrivate) { /* the GPU's alone: a copy the CPU can read */
                static id<MTLCommandQueue> queue;
                static id<MTLTexture> copy;
                if (!queue || queue.device != t.device) queue = [t.device newCommandQueue], copy = nil;
                if (!copy || copy.width != t.width || copy.height != t.height || copy.pixelFormat != t.pixelFormat) {
                    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];
                    d.storageMode = MTLStorageModeShared;
                    copy = [t.device newTextureWithDescriptor:d];
                }
                if (!queue || !copy) return -1;
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
                            toTexture:copy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                from = copy;
            }
            [from getBytes:out bytesPerRow:(NSUInteger)tw * 4 fromRegion:MTLRegionMake2D(0, 0, tw, th) mipmapLevel:0];
        }
        if (rgba)
            for (size_t i = 0, n = (size_t)tw * th; i < n; i++) out[i] = (out[i] & 0xff00ff00u) | (out[i] >> 16 & 0xff) | (out[i] & 0xff) << 16;
        *w = tw, *h = th;
        return 0;
    }
}

/* The IOSurface behind the texture, retained (the caller releases it), or NULL if it has none: what a layer can
   show as it is, with nothing copied. */
IOSurfaceRef hv_metal_texture_surface(void *texture) {
    @autoreleasepool {
        id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
        IOSurfaceRef s = t ? t.iosurface : NULL;
        if (s) CFRetain(s);
        return s;
    }
}
