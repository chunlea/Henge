/* Intel HD Audio: the controller's registers, its command and response rings, and a codec with one output. */
#include "hda.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"

enum {
    BAR_SIZE = 0x4000,
    IN_STREAMS = 4,
    GCAP = 0x4401,           /* four out, four in, 64-bit addresses */
    BCIS = 1 << 2, FIFORDY = 1 << 5,
    AHEAD_MS = 40,           /* how far ahead of the playing the guest's buffer is read */
    STALL_NS = 150000000,    /* a backend that has taken nothing for this long is not playing */
    NID_ROOT = 0, NID_AFG = 1, NID_DAC = 2, NID_PIN = 3,
    PCM_CAPS = 1 << 17 | 1 << 6 | 1 << 5, /* 16 bits; 48 and 44.1 kHz */
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int logging(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_HDA_LOG") ? atoi(getenv("HV_HDA_LOG")) : 0;
    return on;
}

/* ---- interrupts ---- */

static uint32_t intsts(HvHda *d) {
    uint32_t v = 0;
    for (int i = 0; i < HV_HDA_STREAMS; i++)
        if (d->st[i].sts & d->st[i].ctl & 0x1c) v |= 1u << i;
    if ((d->rirbsts & 1 && d->rirbctl & 1) || (d->statests & d->wakeen)) v |= 1u << 30;
    if (v) v |= 1u << 31;
    return v;
}

static void update_irq(HvHda *d) {
    uint32_t s = intsts(d);
    int level = d->intctl >> 31 && ((s >> 30 & 1 && d->intctl >> 30 & 1) || (s & d->intctl & 0xff));
    hv_pci_set_intx(&d->pci, level);
}

/* ---- the codec ---- */

static uint32_t codec_param(int nid, uint32_t id) {
    switch (nid) {
    case NID_ROOT:
        if (id == 0x00) return 0x1af40010; /* vendor and device: QEMU's output codec's */
        if (id == 0x02) return 0x00100101;
        if (id == 0x04) return NID_AFG << 16 | 1;
        break;
    case NID_AFG:
        if (id == 0x04) return NID_DAC << 16 | 2;
        if (id == 0x05) return 0x01;        /* an audio function group */
        if (id == 0x08) return 0x00000808;
        if (id == 0x0a) return PCM_CAPS;
        if (id == 0x0b) return 0x01;        /* PCM */
        if (id == 0x0f) return 0x0f;        /* power states D0 to D3 */
        break;
    case NID_DAC:
        if (id == 0x09) return 0x00000011;  /* an audio output: stereo, with its own formats */
        if (id == 0x0a) return PCM_CAPS;
        if (id == 0x0b) return 0x01;
        break;
    case NID_PIN:
        if (id == 0x09) return 0x00400101;  /* a pin: stereo, with a connection list */
        if (id == 0x0c) return 0x10;        /* an output */
        if (id == 0x0e) return 1;
        break;
    }
    return 0;
}

static uint32_t codec_verb(HvHda *d, uint32_t cmd) {
    int nid = cmd >> 20 & 0x7f;
    uint32_t verb = cmd >> 8 & 0xfff, data = cmd & 0xff;
    if (cmd >> 28 || nid > NID_PIN) return 0;
    switch (verb >> 8) { /* the verbs with 16 bits of data */
    case 0x2: if (nid == NID_DAC) d->dac_fmt = cmd & 0xffff; return 0;
    case 0xa: return nid == NID_DAC ? d->dac_fmt : 0;
    case 0x3: case 0xb: return 0; /* no amplifiers */
    }
    switch (verb) {
    case 0xf00: return codec_param(nid, data);
    case 0xf02: return nid == NID_PIN ? NID_DAC : 0;
    case 0xf05: return d->power[nid] << 4 | d->power[nid];
    case 0x705: d->power[nid] = data & 0xf; return 0;
    case 0xf06: return nid == NID_DAC ? d->dac_stream : 0;
    case 0x706: if (nid == NID_DAC) d->dac_stream = data; return 0;
    case 0xf07: return nid == NID_PIN ? d->pin_ctl : 0;
    case 0x707: if (nid == NID_PIN) d->pin_ctl = data; return 0;
    case 0xf08: return d->unsol[nid];
    case 0x708: d->unsol[nid] = data; return 0;
    case 0xf09: return nid == NID_PIN ? 0x80000000u : 0; /* something is plugged in */
    case 0xf0c: return nid == NID_PIN ? d->eapd : 0;
    case 0x70c: if (nid == NID_PIN) d->eapd = data; return 0;
    case 0xf1c: return nid == NID_PIN ? d->pin_cfg : 0;
    case 0x71c: case 0x71d: case 0x71e: case 0x71f:
        if (nid == NID_PIN) {
            int shift = (int)(verb - 0x71c) * 8;
            d->pin_cfg = (d->pin_cfg & ~(0xffu << shift)) | data << shift;
        }
        return 0;
    case 0xf20: return nid == NID_AFG ? 0x1af40010 : 0;
    case 0x7ff: /* the function group's reset */
        d->dac_stream = 0, d->dac_fmt = 0x0011, d->pin_ctl = 0x40;
        return 0;
    }
    return 0;
}

/* ---- the command and response rings ---- */

static void corb_run(HvHda *d) {
    if (!(d->corbctl & 2) || !(d->rirbctl & 2)) return;
    uint32_t cnt = d->rintcnt & 0xff ? d->rintcnt & 0xff : 256;
    while ((d->corbrp & 0xff) != (d->corbwp & 0xff)) {
        if (d->rirb_count >= cnt) break; /* the driver has responses to take first */
        uint16_t rp = (d->corbrp + 1) & 0xff;
        uint32_t *cmd = hv_phys_ptr(d->m, d->corb + rp * 4, 4);
        if (!cmd) break;
        d->corbrp = rp;
        uint32_t c, r;
        memcpy(&c, cmd, 4);
        r = codec_verb(d, c);
        if (logging()) fprintf(stderr, "hda: verb %08x -> %08x\n", c, r);
        d->rirbwp = (d->rirbwp + 1) & 0xff;
        uint64_t pa = d->rirb + d->rirbwp * 8;
        uint32_t *resp = hv_phys_ptr(d->m, pa, 8);
        if (resp) {
            uint32_t pair[2] = {r, c >> 28};
            memcpy(resp, pair, 8);
            hv_dma_written(d->m, pa, 8);
        }
        d->rirb_count++;
        if (d->rirbctl & 1 && (d->rirb_count == cnt || (d->corbrp & 0xff) == (d->corbwp & 0xff))) d->rirbsts |= 1;
    }
}

/* ---- streams ---- */

static void ring_write(HvHda *d, const uint8_t *p, uint32_t n) {
    uint64_t w = atomic_load_explicit(&d->written, memory_order_relaxed);
    for (uint32_t off = 0; off < n;) {
        uint32_t at = (uint32_t)((w + off) % HV_HDA_RING), k = n - off;
        if (k > HV_HDA_RING - at) k = HV_HDA_RING - at;
        memcpy(d->ring + at, p + off, k);
        off += k;
    }
    atomic_store_explicit(&d->written, w + n, memory_order_release);
}

uint32_t hv_hda_pull(void *opaque, void *buf, uint32_t len) {
    HvHda *d = opaque;
    uint64_t c = atomic_load_explicit(&d->consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&d->written, memory_order_acquire);
    uint32_t n = w - c < len ? (uint32_t)(w - c) : len;
    for (uint32_t off = 0; off < n;) {
        uint32_t at = (uint32_t)((c + off) % HV_HDA_RING), k = n - off;
        if (k > HV_HDA_RING - at) k = HV_HDA_RING - at;
        memcpy((uint8_t *)buf + off, d->ring + at, k);
        off += k;
    }
    memset((uint8_t *)buf + n, 0, len - n);
    atomic_store_explicit(&d->consumed, c + n, memory_order_release);
    return n;
}

/* Move a stream on by n bytes of its cyclic buffer: to the ring if it is the one heard. */
static void advance(HvHda *d, HvHdaStream *s, uint32_t n, int heard) {
    if (!s->bdl || !s->cbl) return;
    for (int guard = 0; n && guard < 4096; guard++) {
        uint8_t *e = hv_phys_ptr(d->m, s->bdl + (uint64_t)s->entry * 16, 16);
        if (!e) return;
        uint64_t addr;
        uint32_t len, flags;
        memcpy(&addr, e, 8), memcpy(&len, e + 8, 4), memcpy(&flags, e + 12, 4);
        uint32_t k = len > s->entry_off ? len - s->entry_off : 0;
        if (k > n) k = n;
        if (k && heard) {
            uint8_t *p = hv_phys_ptr(d->m, addr + s->entry_off, k);
            if (p) ring_write(d, p, k);
            if (p && logging())
                for (uint32_t j = 0; j + 1 < k; j += 2) {
                    int16_t v;
                    memcpy(&v, p + j, 2);
                    if (abs(v) > d->peak) d->peak = abs(v);
                }
        }
        s->entry_off += k;
        s->lpib += k;
        if (s->lpib >= s->cbl) s->lpib -= s->cbl;
        n -= k;
        if (s->entry_off >= len) {
            if (flags & 1) s->sts |= BCIS;
            s->entry = s->entry >= s->lvi ? 0 : s->entry + 1;
            s->entry_off = 0;
        }
    }
    if (d->dp & 1) { /* the position buffer: each stream's position, where a driver may look instead */
        uint64_t pa = (d->dp & ~0x7full) + (uint64_t)(s - d->st) * 8;
        uint32_t *p = hv_phys_ptr(d->m, pa, 4);
        if (p) {
            memcpy(p, &s->lpib, 4);
            hv_dma_written(d->m, pa, 4);
        }
    }
}

/* The stream the codec's output is listening to (its converter's stream number), 16 bits a sample. */
static int heard(HvHda *d, HvHdaStream *s) {
    uint32_t tag = s->ctl >> 20 & 0xf;
    return s - d->st >= IN_STREAMS && tag && tag == d->dac_stream >> 4 && (s->fmt >> 4 & 7) == 1;
}

static void unhear(HvHda *d, HvHdaStream *s) {
    if (!s->audible) return;
    if (d->backend_on && d->backend.stop) d->backend.stop(d->backend.opaque);
    d->backend_on = 0;
    s->audible = 0;
    s->clock_ns = now_ns();
    if (d->m->log || logging())
        fprintf(stderr, "hengevm: hda: stream %d is no longer played: %.2f s of sound, %.2f s more dropped, loudest sample %d\n", (int)(s - d->st),
                (double)(d->played - d->heard_played) / (s->rate * s->frame_bytes), (double)(d->dropped - d->heard_dropped) / (s->rate * s->frame_bytes), d->peak);
}

/* Windows leaves its stream running and turns the codec's converter to it only while there is sound. */
static void hear(HvHda *d, HvHdaStream *s) {
    for (int j = 0; j < HV_HDA_STREAMS; j++)
        if (d->st[j].audible) return;
    /* What was left in the ring is not this stream's. */
    atomic_store(&d->written, atomic_load(&d->consumed));
    d->seen_consumed = atomic_load(&d->consumed), d->seen_ns = now_ns();
    d->heard_played = d->played, d->heard_dropped = d->dropped, d->peak = 0;
    d->backend_on = d->backend.start && d->backend.start(d->backend.opaque, (s->fmt & 0xf) + 1, HV_VSND_FMT_S16, s->rate) == 0;
    s->audible = 1;
    if (d->m->log || logging())
        fprintf(stderr, "hengevm: hda: stream %d is played: %u Hz, %u channels%s\n", (int)(s - d->st), s->rate, (s->fmt & 0xf) + 1,
                d->backend_on ? "" : " (no output for it: by the clock)");
}

static void stream_stop(HvHda *d, HvHdaStream *s) {
    unhear(d, s);
    s->running = 0;
    s->sts &= (uint8_t)~FIFORDY;
}

static void stream_start(HvHda *d, HvHdaStream *s) {
    int i = (int)(s - d->st);
    unsigned base = s->fmt >> 14 & 1 ? 44100 : 48000, mult = (s->fmt >> 11 & 7) + 1, div = (s->fmt >> 8 & 7) + 1;
    unsigned bits = s->fmt >> 4 & 7, channels = (s->fmt & 0xf) + 1;
    static const unsigned bytes[8] = {1, 2, 4, 4, 4, 2, 2, 2};
    s->rate = base * mult / div;
    s->frame_bytes = bytes[bits] * channels;
    s->entry = s->entry_off = 0;
    s->lpib = 0;
    s->running = 1;
    s->sts |= FIFORDY;
    s->clock_ns = now_ns();
    if (d->m->log || logging()) fprintf(stderr, "hengevm: hda: stream %d runs: %u Hz, %u channels, %u bytes a frame\n", i, s->rate, channels, s->frame_bytes);
}

static void stream_ctl(HvHda *d, HvHdaStream *s) {
    if (s->ctl & 1) { /* in reset */
        stream_stop(d, s);
        uint32_t keep = s->ctl & 1;
        memset(s, 0, sizeof *s);
        s->ctl = keep;
        return;
    }
    int run = s->ctl >> 1 & 1;
    if (run && !s->running) stream_start(d, s);
    else if (!run && s->running) stream_stop(d, s);
}

void hv_hda_poll(HvHda *d) {
    if (!d) return;
    uint64_t now = 0;
    int any = 0;
    for (int i = 0; i < HV_HDA_STREAMS; i++) {
        HvHdaStream *s = &d->st[i];
        if (!s->running || !s->frame_bytes || !s->rate) continue;
        if (!now) now = now_ns();
        any = 1;
        uint64_t per_s = (uint64_t)s->rate * s->frame_bytes;
        int by_clock = 1;
        if (heard(d, s) && !s->audible) hear(d, s);
        else if (!heard(d, s)) unhear(d, s);
        if (s->audible && d->backend_on) {
            uint64_t c = atomic_load_explicit(&d->consumed, memory_order_acquire);
            uint64_t w = atomic_load_explicit(&d->written, memory_order_relaxed);
            if (c != d->seen_consumed) {
                if (now - d->seen_ns > STALL_NS) s->clock_ns = now; /* it is playing again */
                d->seen_consumed = c, d->seen_ns = now;
            }
            if (now - d->seen_ns <= STALL_NS) {
                uint64_t want = c + per_s * AHEAD_MS / 1000;
                if (want > w) {
                    uint32_t n = (uint32_t)(want - w);
                    n -= n % s->frame_bytes;
                    advance(d, s, n, 1);
                    d->played += n;
                }
                s->clock_ns = now;
                by_clock = 0;
            }
        }
        if (by_clock) {
            uint64_t n = (now - s->clock_ns) * per_s / 1000000000ull;
            n -= n % s->frame_bytes;
            if (n > per_s / 4) n = per_s / 4 - per_s / 4 % s->frame_bytes; /* after a long pause: not all at once */
            if (n) {
                advance(d, s, (uint32_t)n, 0);
                s->clock_ns += n * 1000000000ull / per_s;
                if (now - s->clock_ns > 250000000ull) s->clock_ns = now;
                d->dropped += n;
            }
        }
    }
    if (any) update_irq(d);
}

/* ---- registers ---- */

static void reset(HvHda *d) {
    for (int i = 0; i < HV_HDA_STREAMS; i++) {
        stream_stop(d, &d->st[i]);
        memset(&d->st[i], 0, sizeof d->st[i]);
    }
    d->gctl = d->intctl = d->ssync = d->icoi = d->icii = 0;
    d->wakeen = d->statests = d->corbwp = d->corbrp = d->rirbwp = d->rintcnt = d->icis = 0;
    d->corbctl = d->corbsts = d->rirbctl = d->rirbsts = 0;
    d->corb = d->rirb = d->dp = 0;
    d->rirb_count = 0;
    d->dac_stream = 0, d->dac_fmt = 0x0011, d->pin_ctl = 0x40, d->pin_cfg = 0x90170110, d->eapd = 2;
    memset(d->unsol, 0, sizeof d->unsol), memset(d->power, 0, sizeof d->power);
}

#define BYTE(v, i) ((uint8_t)((uint64_t)(v) >> ((i) * 8)))
#define SET(field, i, b, type) ((field) = (type)(((uint64_t)(field) & ~(0xffull << ((i) * 8))) | (uint64_t)(b) << ((i) * 8)))

static uint8_t read8(HvHda *d, uint32_t a) {
    if (a >= 0x2000) a -= 0x2000; /* the registers' second copy (the wall clock and the streams' positions, for mapping to programs): where Windows reads positions */
    if (a >= 0x80 && a < 0x80 + HV_HDA_STREAMS * 0x20) {
        HvHdaStream *s = &d->st[(a - 0x80) / 0x20];
        uint32_t r = (a - 0x80) % 0x20;
        if (r < 3) return BYTE(s->ctl, r);
        if (r == 3) return s->sts;
        if (r < 8) return BYTE(s->lpib, r - 4);
        if (r < 0xc) return BYTE(s->cbl, r - 8);
        if (r < 0xe) return BYTE(s->lvi, r - 0xc);
        if (r >= 0x10 && r < 0x12) return BYTE(0x100, r - 0x10); /* the FIFO's size */
        if (r >= 0x12 && r < 0x14) return BYTE(s->fmt, r - 0x12);
        if (r >= 0x18) return BYTE(s->bdl, r - 0x18);
        return 0;
    }
    switch (a) {
    case 0x00: case 0x01: return BYTE(GCAP, a);
    case 0x03: return 1; /* version 1.0 */
    case 0x04: return 0x3c;
    case 0x06: return 0x1d;
    case 0x08: case 0x09: case 0x0a: case 0x0b: return BYTE(d->gctl, a - 0x08);
    case 0x0c: case 0x0d: return BYTE(d->wakeen, a - 0x0c);
    case 0x0e: case 0x0f: return BYTE(d->statests, a - 0x0e);
    case 0x20: case 0x21: case 0x22: case 0x23: return BYTE(d->intctl, a - 0x20);
    case 0x24: case 0x25: case 0x26: case 0x27: return BYTE(intsts(d), a - 0x24);
    case 0x30: case 0x31: case 0x32: case 0x33: return BYTE((now_ns() - d->t0_ns) * 24 / 1000, a - 0x30); /* 24 MHz */
    case 0x38: case 0x39: case 0x3a: case 0x3b: return BYTE(d->ssync, a - 0x38);
    case 0x48: case 0x49: return BYTE(d->corbwp, a - 0x48);
    case 0x4a: case 0x4b: return BYTE(d->corbrp, a - 0x4a);
    case 0x4c: return d->corbctl;
    case 0x4d: return d->corbsts;
    case 0x4e: case 0x5e: return 0x42; /* 256 entries, and no other size */
    case 0x58: case 0x59: return BYTE(d->rirbwp, a - 0x58);
    case 0x5a: case 0x5b: return BYTE(d->rintcnt, a - 0x5a);
    case 0x5c: return d->rirbctl;
    case 0x5d: return d->rirbsts;
    case 0x60: case 0x61: case 0x62: case 0x63: return BYTE(d->icoi, a - 0x60);
    case 0x64: case 0x65: case 0x66: case 0x67: return BYTE(d->icii, a - 0x64);
    case 0x68: case 0x69: return BYTE(d->icis, a - 0x68);
    }
    if (a >= 0x40 && a < 0x48) return BYTE(d->corb, a - 0x40);
    if (a >= 0x50 && a < 0x58) return BYTE(d->rirb, a - 0x50);
    if (a >= 0x70 && a < 0x78) return BYTE(d->dp, a - 0x70);
    return 0;
}

static void write8(HvHda *d, uint32_t a, uint8_t b) {
    if (a >= 0x80 && a < 0x80 + HV_HDA_STREAMS * 0x20) {
        HvHdaStream *s = &d->st[(a - 0x80) / 0x20];
        uint32_t r = (a - 0x80) % 0x20;
        if (r == 0) SET(s->ctl, 0, b & 0x1f, uint32_t);
        else if (r == 2) SET(s->ctl, 2, b, uint32_t);
        else if (r == 3) s->sts &= (uint8_t)~(b & 0x1c);
        else if (r >= 8 && r < 0xc) SET(s->cbl, r - 8, b, uint32_t);
        else if (r >= 0xc && r < 0xe) SET(s->lvi, r - 0xc, r == 0xc ? b : 0, uint16_t);
        else if (r >= 0x12 && r < 0x14) SET(s->fmt, r - 0x12, b, uint16_t);
        else if (r >= 0x18) SET(s->bdl, r - 0x18, r == 0x18 ? b & 0x80 : b, uint64_t);
        return;
    }
    switch (a) {
    case 0x08: case 0x09: case 0x0a: case 0x0b: SET(d->gctl, a - 0x08, b, uint32_t); return;
    case 0x0c: case 0x0d: SET(d->wakeen, a - 0x0c, b, uint16_t); return;
    case 0x0e: case 0x0f: d->statests &= (uint16_t)~((uint16_t)b << ((a - 0x0e) * 8)); return;
    case 0x20: case 0x21: case 0x22: case 0x23: SET(d->intctl, a - 0x20, b, uint32_t); return;
    case 0x38: case 0x39: case 0x3a: case 0x3b: SET(d->ssync, a - 0x38, b, uint32_t); return;
    case 0x48: d->corbwp = b; return;
    case 0x4a: d->corbrp = (uint16_t)((d->corbrp & 0xff00) | b); return; /* read-only but for the reset bit */
    case 0x4b:
        if (b & 0x80) d->corbrp = 0x8000;       /* reset: the pointer is 0, and the bit reads back until cleared */
        else d->corbrp &= 0x00ff;
        return;
    case 0x4c: d->corbctl = b & 3; return;
    case 0x4d: d->corbsts &= (uint8_t)~(b & 1); return;
    case 0x59: if (b & 0x80) d->rirbwp = 0; return;
    case 0x5a: d->rintcnt = b; return;
    case 0x5c: d->rirbctl = b & 7; return;
    case 0x5d:
        d->rirbsts &= (uint8_t)~(b & 5);
        if (b & 1) d->rirb_count = 0;
        return;
    case 0x60: case 0x61: case 0x62: case 0x63: SET(d->icoi, a - 0x60, b, uint32_t); return;
    case 0x68:
        if (b & 2) d->icis &= (uint16_t)~2;
        if (b & 1) d->icis |= 1;
        return;
    }
    if (a >= 0x40 && a < 0x48) SET(d->corb, a - 0x40, a == 0x40 ? b & 0x80 : b, uint64_t);
    else if (a >= 0x50 && a < 0x58) SET(d->rirb, a - 0x50, a == 0x50 ? b & 0x80 : b, uint64_t);
    else if (a >= 0x70 && a < 0x78) SET(d->dp, a - 0x70, a == 0x70 ? b & 0x81 : b, uint64_t);
}

static uint64_t bar_read(HvPciDev *p, int bar, uint64_t off, unsigned size) {
    HvHda *d = p->opaque;
    uint64_t v = 0;
    if (bar != 0 || off + size > BAR_SIZE) return ~0ull;
    for (unsigned i = 0; i < size; i++) v |= (uint64_t)read8(d, (uint32_t)off + i) << (i * 8);
    if (logging() > 2 && off != 0x58 && off != 0x24 && off != 0x5d) fprintf(stderr, "hda: read %#llx -> %#llx (%u)\n", (unsigned long long)off, (unsigned long long)v, size);
    return v;
}

static void bar_write(HvPciDev *p, int bar, uint64_t off, uint64_t v, unsigned size) {
    HvHda *d = p->opaque;
    if (bar != 0 || off + size > BAR_SIZE) return;
    uint32_t a = (uint32_t)off, end = a + size, was = d->gctl;
    if (logging() > 1 && a != 0x48) fprintf(stderr, "hda: write %#x = %#llx (%u)\n", a, (unsigned long long)v, size);
    for (unsigned i = 0; i < size; i++) write8(d, a + i, (uint8_t)(v >> (i * 8)));
    if (a < 0x0c && end > 0x08) { /* GCTL: bit 0 is the controller's reset, held while it is 0 */
        if (!(d->gctl & 1)) reset(d);
        else if (!(was & 1)) d->statests |= 1; /* out of reset: the codec at address 0 has made itself known */
    }
    if (a < 0x6a && end > 0x68 && d->icis & 1) { /* a command given directly */
        d->icii = codec_verb(d, d->icoi);
        d->icis = 2;
    }
    if (a >= 0x80 && a < 0x80 + HV_HDA_STREAMS * 0x20 && (a - 0x80) % 0x20 < 3) stream_ctl(d, &d->st[(a - 0x80) / 0x20]);
    if (a < 0x60 && end > 0x48) corb_run(d);
    update_irq(d);
}

HvHda *hv_hda_create(HvMachine *m, HvPcie *bus) {
    HvHda *d = calloc(1, sizeof *d);
    if (!d || !(d->ring = calloc(1, HV_HDA_RING))) {
        free(d);
        return NULL;
    }
    d->m = m;
    d->t0_ns = now_ns();
    reset(d);
    hv_pci_init(&d->pci, 0x8086, 0x2668, 0x04030001, 0x1af4, 0x1100); /* Intel's ICH6 controller, as QEMU's intel-hda */
    d->pci.opaque = d;
    d->pci.bar_read = bar_read;
    d->pci.bar_write = bar_write;
    hv_pci_add_bar(&d->pci, 0, BAR_SIZE, 0);
    if (hv_pcie_attach(bus, &d->pci) < 0) {
        free(d->ring), free(d);
        return NULL;
    }
    return d;
}

void hv_hda_destroy(HvHda *d) {
    if (!d) return;
    free(d->ring);
    free(d);
}
