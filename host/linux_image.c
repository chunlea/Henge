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

/* The decoder's output gathered into whole blocks: a block of zeros is a hole,
   whatever pieces the decoder handed it over in. */
typedef struct Sink {
    int fd;
    uint64_t pos; /* where blk goes */
    size_t fill;
    uint64_t zero_start, zero_len; /* the current run of zero blocks */
    uint64_t (*runs)[2];           /* the finished runs, punched at the end */
    size_t nruns, cap;
    uint8_t blk[BLOCK];
} Sink;

/* APFS allocates some of a gap that writes merely skip over (a 112 KB gap took
   about half its size), and punching while the file still grows does not help
   either, so the zero runs are noted and punched out once the file is whole. */
static void punch(Sink *k) {
    if (!k->zero_len) return;
    if (k->nruns == k->cap) {
        size_t c = k->cap ? k->cap * 2 : 4096;
        void *n = realloc(k->runs, c * sizeof *k->runs);
        if (!n) { k->zero_len = 0; return; } /* only space is lost */
        k->runs = n, k->cap = c;
    }
    k->runs[k->nruns][0] = k->zero_start, k->runs[k->nruns][1] = k->zero_len;
    k->nruns++;
    k->zero_len = 0;
}

static void punch_all(Sink *k) {
    punch(k);
#ifdef F_PUNCHHOLE
    for (size_t i = 0; i < k->nruns; i++) {
        fpunchhole_t h = {0, 0, (off_t)k->runs[i][0], (off_t)k->runs[i][1]};
        (void)fcntl(k->fd, F_PUNCHHOLE, &h); /* only space: the bytes read as zero either way */
    }
#endif
}

static int flush_block(Sink *k) {
    if (k->fill && all_zero(k->blk, k->fill)) {
        if (!k->zero_len) k->zero_start = k->pos;
        k->zero_len += k->fill;
    } else if (k->fill) {
        punch(k);
        for (size_t at = 0; at < k->fill;) {
            ssize_t w = pwrite(k->fd, k->blk + at, k->fill - at, (off_t)(k->pos + at));
            if (w <= 0) return -1;
            at += (size_t)w;
        }
    }
    k->pos += k->fill;
    k->fill = 0;
    return 0;
}

static int put(Sink *k, const uint8_t *p, size_t n) {
    while (n) {
        size_t take = BLOCK - k->fill < n ? BLOCK - k->fill : n;
        memcpy(k->blk + k->fill, p, take);
        k->fill += take, p += take, n -= take;
        if (k->fill == BLOCK && flush_block(k)) return -1;
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
    Sink *sink = calloc(1, sizeof *sink);
    size_t last = 0;
    if (z && inBuf && outBuf && sink) {
        sink->fd = out;
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
                    if (put(sink, outBuf, ob.pos)) { why = "The disk could not be written (not enough space?)."; break; }
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
            else if (put(sink, outBuf, ob.pos)) why = "The disk could not be written (not enough space?).";
            else if (!ob.pos) { if (last) why = "The disk image ends early (a part is missing or cut short)."; break; }
        }
        if (!why && flush_block(sink)) why = "The disk could not be written (not enough space?).";
        if (!why && ftruncate(out, (off_t)sink->pos) != 0) why = "The disk could not be written (not enough space?).";
        if (!why) punch_all(sink);
        if (!why && fsync(out) != 0) why = "The disk could not be written (not enough space?).";
        if (!why) r = 0;
    }
    ZSTD_freeDStream(z);
    if (sink) free(sink->runs);
    free(inBuf), free(outBuf), free(sink);
    close(out);
    if (r != 0) {
        unlink(out_path);
        return fail(err, errlen, why);
    }
    return 0;
}
