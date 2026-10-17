/*
 * usbaudio.h - USB Audio Class 1.0 / 2.0 and USB MIDI class driver,
 * interface for clients (the AUDIONT sound driver on NT, the test kernel).
 *
 * Streams are pull/push: the class driver keeps isochronous transfers in
 * flight on its own and calls the client to produce (playback) or consume
 * (capture) samples in the device's format.
 */

#ifndef USBAUDIO_H
#define USBAUDIO_H

#include "usbnt.h"

#include "usbntaud.h"

/* worker / passive context */
int UA_CALL UaRegister(const UA_CLIENT *Client);           /* reports present devices too */
void UA_CALL UaUnregister(void);
int UA_CALL UaGetInfo(ULONG Id, UA_INFO *Info);
int UA_CALL UaOpen(ULONG Id, ULONG Dir, ULONG Rate, ULONG Channels, ULONG Bits,
               const UA_STREAM_OPS *Ops, UA_STREAM_INFO *Out);
void UA_CALL UaClose(ULONG Id, ULONG Dir);
/* any level up to dispatch */
int UA_CALL UaStart(ULONG Id, ULONG Dir);
void UA_CALL UaStop(ULONG Id, ULONG Dir);
int UA_CALL UaSetVolume(ULONG Id, ULONG Dir, SHORT Left, SHORT Right, BOOLEAN Mute);   /* deferred */
int UA_CALL UaSelectInput(ULONG Id, ULONG Index);                                     /* deferred */
void UA_CALL UaGetStats(ULONG Id, ULONG Dir, UA_STATS *Stats);
int UA_CALL UaMidiOut(ULONG Id, ULONG Cable, const UCHAR *Data, ULONG Len);

/* number of audio devices present (for diagnostics) */
ULONG   UaDeviceCount(void);

#endif
