/*
 * ps2pad.h - native protocols of PS/2 touch pads (ps2pad.c): Synaptics,
 * Elantech v2 - v4, ALPS v3 / v5 / v7; detection, mode set-up and packet
 * decoding, portable (NT 3.x / 4.0 controller driver ntps2.c, the Windows
 * 2000 / XP i8042prt filter wdmps2.c, the test harness).  Gestures come from
 * the touch pad engine (ptp.c); trackpoints behind the pad give relative
 * motion, middle button + motion = wheel.
 */
#ifndef PS2PAD_H
#define PS2PAD_H

#include "usbinput.h"
#include "ptp.h"

#define PS2PAD_NONE         0
#define PS2PAD_SYNAPTICS    1
#define PS2PAD_ELANTECH     2
#define PS2PAD_ALPS         3

/* synchronous access to the pointing device, used only while detecting / setting up */
typedef struct _PS2PAD_IO {
    void   *Ctx;
    BOOLEAN (*Send)(void *Ctx, UCHAR B);        /* byte to the device; TRUE when it answers ACK (FA) */
    int     (*Read)(void *Ctx, ULONG Ms);       /* next byte from the device, -1 when none came within Ms */
} PS2PAD_IO;

typedef struct _PS2PAD {
    UCHAR   Proto;              /* PS2PAD_* */
    UCHAR   Ver;                /* Elantech hardware version 2 - 4; ALPS 3 (Pinnacle), 0x31 (Rushmore), 5, 7 */
    UCHAR   PktSize, Count;
    UCHAR   Pkt[8];
    UCHAR   Touching, Stick, ClickPad, AQuirk;
    /* Synaptics */
    ULONG   Ident, Model, Caps, ExtCap, Ext0C, Ext10;
    UCHAR   SynMode, Agm, AgmCount, Pad2;
    LONG    AgmX, AgmY;
    /* Elantech */
    ULONG   Fw;
    UCHAR   ECap[3], ESamples[3], Crc, IcVer;
    UCHAR   MtValid, MtDown, Pad3[2];
    LONG    MtX[5], MtY[5];
    /* ALPS */
    UCHAR   Byte0, Mask0, XBits, YBits;
    UCHAR   MultiPacket, SecondTouch, Pad4[2];
    UCHAR   MultiData[6], Pad5[2];
    ULONG   AFlags;
    /* every pad */
    LONG    MinX, MaxX, MinY, MaxY;
    UCHAR   PadButtons, StickButtons, StickMid, StickScrolled;
    UCHAR   LastOut, Pad6[3];
    LONG    StickAcc;
    PTP_OUT Out;
    void   *OutCtx;
    ULONG   Packets, Resyncs;
    char    Name[40];
    char    Info[128];          /* what the detection found (for the caller's log) */
    PTP_STATE Ptp;
} PS2PAD;

/* After a reset of the device (FF): identifies the touch pad and switches it to its
   native (absolute) mode; data reporting stays off (the caller sends F4, then calls
   Ps2PadStarted).  Returns PS2PAD_* (PS2PAD_NONE: no supported pad, the device must be
   reset again for the plain mouse protocols).  Out gets the pointer events. */
int     Ps2PadDetect(PS2PAD *P, const PS2PAD_IO *Io, PTP_OUT Out, void *Ctx);
/* data reporting is on: trackpoint behind a Synaptics pad (pass-through port) on too */
void    Ps2PadStarted(PS2PAD *P, const PS2PAD_IO *Io);
/* a byte of the data stream (DPC level) */
void    Ps2PadByte(PS2PAD *P, UCHAR B);
/* every 10 - 50 ms (same context as Ps2PadByte): tap clicks end */
void    Ps2PadTick(PS2PAD *P);
BOOLEAN Ps2PadBusy(const PS2PAD *P);
/* a pointer event of Out (buttons state) as a USB_MOUSE_EVENT; *Prev keeps the buttons */
BOOLEAN Ps2PadEvent(UCHAR *Prev, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan, USB_MOUSE_EVENT *E);

#endif
