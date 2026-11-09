// SPDX-License-Identifier: GPL-2.0-or-later
#include "linux_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zstd.h"

#define BLOCK 16384 /* holes are made in whole blocks of this size */

static int fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return -1;
}

static int all_zero(const uint8_t *p, size_t n) {
    const uint64_t *q = (const uint64_t *)(const void *)p;
    for (size_t i = 0; i < n / 8; i++) if (q[i]) return 0;
    for (size_t i = n & ~(size_t)7; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* Writes what the decoder produced at *pos, leaving zero blocks as holes. */
static int put(int fd, const uint8_t *p, size_t n, uint64_t *pos) {
    while (n) {
        size_t in_block = BLOCK - (size_t)(*pos % BLOCK), take = n < in_block ? n : in_block;
        if (!(take == BLOCK && all_zero(p, take))) {
            for (size_t at = 0; at < take;) {
                ssize_t w = pwrite(fd, p + at, take - at, (off_t)(*pos + at));
                if (w <= 0) return -1;
                at += (size_t)w;
            }
        }
        *pos += take, p += take, n -= take;
    }
    return 0;
}

int hv_linux_image_unpack(const char *const *parts, int nparts, const char *out_path, int remove_parts,
                          void (*progress)(uint64_t, uint64_t, void *), void *user, char *err, size_t errlen) {
    uint64_t total = 0, done = 0;
    for (int i = 0; i < nparts; i++) {
        struct stat st;
        if (stat(parts[i], &st) != 0) return fail(err, errlen, "A part of the disk image is missing.");
        total += (uint64_t)st.st_size;
    }
    int out = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (out < 0) return fail(err, errlen, "Cannot create the disk.");
    ZSTD_DStream *z = ZSTD_createDStream();
    size_t inCap = ZSTD_DStreamInSize(), outCap = ZSTD_DStreamOutSize() * 8;
    uint8_t *inBuf = malloc(inCap), *outBuf = malloc(outCap);
    int r = -1;
    const char *why = "Not enough memory to unpack the disk.";
    uint64_t pos = 0;
    size_t last = 0;
    if (z && inBuf && outBuf) {
        ZSTD_initDStream(z);
        why = NULL;
        for (int i = 0; i < nparts && !why; i++) {
            int in = open(parts[i], O_RDONLY);
            if (in < 0) { why = "Cannot read a part of the disk image."; break; }
            ssize_t n;
            while (!why && (n = read(in, inBuf, inCap)) > 0) {
                ZSTD_inBuffer ib = {inBuf, (size_t)n, 0};
                while (ib.pos < ib.size) {
                    ZSTD_outBuffer ob = {outBuf, outCap, 0};
                    last = ZSTD_decompressStream(z, &ob, &ib);
                    if (ZSTD_isError(last)) { why = "The disk image is damaged (it did not decompress)."; break; }
                    if (put(out, outBuf, ob.pos, &pos)) { why = "The disk could not be written (not enough space?)."; break; }
                }
                done += (uint64_t)n;
                if (progress) progress(done, total, user);
            }
            if (!why && n < 0) why = "Cannot read a part of the disk image.";
            close(in);
            if (!why && remove_parts) unlink(parts[i]);
        }
        /* Drain what the decoder still holds. */
        while (!why && last) {
            ZSTD_inBuffer ib = {NULL, 0, 0};
            ZSTD_outBuffer ob = {outBuf, outCap, 0};
            last = ZSTD_decompressStream(z, &ob, &ib);
            if (ZSTD_isError(last)) why = "The disk image is damaged (it did not decompress).";
            else if (put(out, outBuf, ob.pos, &pos)) why = "The disk could not be written (not enough space?).";
            else if (!ob.pos) { if (last) why = "The disk image ends early (a part is missing or cut short)."; break; }
        }
        if (!why && ftruncate(out, (off_t)pos) != 0) why = "The disk could not be written (not enough space?).";
        if (!why && fsync(out) != 0) why = "The disk could not be written (not enough space?).";
        if (!why) r = 0;
    }
    ZSTD_freeDStream(z);
    free(inBuf), free(outBuf);
    close(out);
    if (r != 0) {
        unlink(out_path);
        return fail(err, errlen, why);
    }
    return 0;
}
