#ifndef HENGEVM_AUDIO_MAC_H
#define HENGEVM_AUDIO_MAC_H

#include "virtio_snd.h"

/* Play the guest's virtio-snd output on the Mac's default output device. */
void hv_audio_attach(HvVirtioSnd *d);
/* Stops and disposes the output unit, waiting for its last render callback. */
void hv_audio_detach(void);

#endif
