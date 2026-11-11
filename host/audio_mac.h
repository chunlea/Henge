#ifndef HENGEVM_AUDIO_MAC_H
#define HENGEVM_AUDIO_MAC_H

#include "virtio_snd.h"

/* Play the guest's virtio-snd output on the Mac's default output device. */
void hv_audio_attach(HvVirtioSnd *d);
/* The same for any device that hands PCM out the same way (the HD Audio controller): its backend slot, and what
   to pull with. One device at a time. */
void hv_audio_attach_pcm(HvVirtioSndBackend *slot, uint32_t (*pull)(void *src, void *buf, uint32_t len), void *src);
/* Stops and disposes the output unit, waiting for its last render callback. */
void hv_audio_detach(void);

#endif
