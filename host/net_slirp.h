#ifndef HENGEVM_NET_SLIRP_H
#define HENGEVM_NET_SLIRP_H

#include "machine.h"
#include "virtio_net.h"

/* Before hv_net_attach: the Unix socket behind the guest's 10.0.2.4:80 (share.c). */
void hv_net_set_share_socket(const char *path);
/* User-mode NAT for the guest's virtio-net (see net_slirp.c). Returns 0 on success. */
int hv_net_attach(HvMachine *m, HvVirtioNet *n);
/* The same for any card: `receive` hands it a frame (0: it cannot take one now, try again later), and
   the card calls *transmit, which this sets, with each frame the guest sends. */
typedef int (*HvNetReceive)(void *card, const uint8_t *frame, uint32_t len);
typedef void (*HvNetTransmit)(void *opaque, const uint8_t *frame, uint32_t len);
int hv_net_attach_card(HvMachine *m, HvNetReceive receive, void *card, HvNetTransmit *transmit);
/* CPUs must be stopped. Joins the network worker before its machine is freed. */
void hv_net_detach(void);
/* CPU thread, from the board's tick: hand frames for the guest to the device. */
void hv_net_poll(void);

extern uint64_t hv_net_tx_frames, hv_net_rx_frames;

#endif
