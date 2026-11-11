#ifndef HENGEVM_HDA_H
#define HENGEVM_HDA_H

#include <stdatomic.h>
#include <stdint.h>

#include "pcie.h"
#include "virtio_snd.h"

/* An Intel High Definition Audio controller (the specification's revision 1.0a) with one codec: a stereo
   output, 16 bits at 44.1 or 48 kHz. It is the sound card Windows has a driver for out of the box (hdaudbus
   and the generic HD Audio class driver), as it has none for virtio-snd.

   The guest's PCM is taken from its cyclic buffer into a ring here as the host plays it (a backend pulls with
   hv_hda_pull, as from the virtio device), a little ahead of the playing; the position the guest reads moves
   with the taking, so the host's audio clock is the guest's. With no backend, or one that has stopped
   asking, the stream moves by the wall clock and its sound is dropped. */
#define HV_HDA_STREAMS 8  /* four in, four out, as QEMU's; the codec only has use for an output one */
#define HV_HDA_RING (1u << 16)

typedef struct HvHdaStream {
    uint32_t ctl;      /* SDCTL, 24 bits */
    uint8_t sts;
    uint32_t lpib, cbl;
    uint16_t lvi, fmt;
    uint64_t bdl;
    uint32_t entry, entry_off; /* where in the buffer list the next byte is */
    int running, audible;
    unsigned frame_bytes, rate;
    uint64_t clock_ns;  /* wall-clock streams: when it was last moved */
} HvHdaStream;

typedef struct HvHda {
    HvPciDev pci;
    HvMachine *m;
    HvVirtioSndBackend backend;
    uint32_t gctl, intctl, ssync, icoi, icii;
    uint16_t wakeen, statests, corbwp, corbrp, rirbwp, rintcnt, icis;
    uint8_t corbctl, corbsts, rirbctl, rirbsts;
    uint64_t corb, rirb, dp;
    uint32_t rirb_count;
    uint64_t t0_ns;
    HvHdaStream st[HV_HDA_STREAMS];
    /* the codec */
    uint32_t dac_stream, dac_fmt, pin_ctl, pin_cfg, unsol[4], power[4], eapd;
    /* the ring: written on the CPU thread, read by the backend */
    uint8_t *ring;
    _Atomic uint64_t written, consumed;
    uint64_t seen_consumed, seen_ns; /* the backend's progress, to tell when it has stopped asking */
    int backend_on;
    uint64_t played, dropped;
    uint64_t heard_played, heard_dropped; /* HV_HDA_LOG: what a spell of sound came to */
    int peak;
} HvHda;

HvHda *hv_hda_create(HvMachine *m, HvPcie *bus);
/* CPU thread, each tick: move the running streams on. */
void hv_hda_poll(HvHda *d);
/* Backend thread: take up to `len` bytes of PCM; the rest of `buf` is silence. Returns bytes taken. */
uint32_t hv_hda_pull(void *d, void *buf, uint32_t len);
void hv_hda_destroy(HvHda *d);

#endif
