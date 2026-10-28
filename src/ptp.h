/*
 * ptp.h - touch pad gestures (ptp.c): one engine for every touch pad,
 * Windows Precision Touchpads (HID over I2C: PtpInit / PtpReport) and pads
 * whose own protocol the caller decodes (PS/2 Synaptics / Elantech / ALPS in
 * ntps2.c, SMBus pads: PtpInitPad / PtpFrame).
 *
 * Contact interface (PtpInitPad / PtpFrame / PtpTick):
 *
 *   PtpInitPad(S, MinX, MaxX, MinY, MaxY, Flags, Out, Ctx)
 *       the pad's coordinate range; Y grows towards the user (down), so a
 *       decoder whose Y grows upwards (Synaptics, Elantech) passes
 *       MaxY + MinY - y.  Flags PTP_PAD_*.  Out is called with every pointer
 *       event (buttons bit 0 left, 1 right, 2 middle; motion in mouse counts,
 *       Y down; wheel in detents, positive = away from the user; pan in
 *       detents, positive = right); Ctx is its first argument.  Reads the
 *       Touchpad* settings (OsGetConfig).
 *   PtpFrame(S, T, N, Buttons)
 *       one complete frame: the N contacts on the pad now (N = 0: none; at
 *       most PTP_MAX_CONTACTS), each with an identifier that stays the same
 *       while the finger is on the pad (a decoder without tracking passes the
 *       slot number).  A semi-mt pad (PTP_PAD_SEMIMT) passes two corners of
 *       the box; a pad that only counts fingers may pass N copies of one
 *       point.  Buttons: the pad's physical buttons, bit 0 left (a ClickPad's
 *       one button), bit 1 right, bit 2 middle.  Called at DPC level or below,
 *       never concurrently for one state.
 *   PtpTick(S, NULL)
 *       every 10 - 50 ms while a tap click is pending (PtpBusy): releases it.
 *
 * The state is plain memory (no allocation, no OS objects); sizeof(PTP_STATE)
 * is a few hundred bytes.
 */
#ifndef PTP_H
#define PTP_H

#include "usbinput.h"

#define PTP_MAX_CONTACTS    10

/* PtpInitPad flags */
#define PTP_PAD_CLICKPAD    0x01    /* the pad is the button: bottom right corner = right, two fingers = right */
#define PTP_PAD_SEMIMT      0x02    /* two contacts are the corners of the fingers' box */
#define PTP_PAD_ONEFINGER   0x04    /* no finger count: edge scrolling instead of two finger scrolling (auto) */

typedef void (*PTP_OUT)(void *Ctx, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan);

typedef struct _PTP_TOUCH {
    ULONG   Id;
    LONG    X, Y;
} PTP_TOUCH;

typedef struct _PTP_CONTACT {
    UCHAR   Used;
    UCHAR   Down;
    UCHAR   Pad[2];
    ULONG   Id;
    LONG    X, Y;
} PTP_CONTACT;

typedef struct _PTP_STATE {
    HID_PTP_LAYOUT L;
    PTP_CONTACT C[PTP_MAX_CONTACTS];
    UCHAR   Tap, ScrollMode, ButtonDown, ClickButtons, TapButtons, LastButtons, TapPossible, TapMax;
    UCHAR   Flags, EdgeScroll, Accel, Edge, PhysButtons, Swipe, SwDone, Pad2;
    ULONG   Speed, ScrollSpeed;
    LONG    MinX, MaxX, MinY, MaxY;
    LONG    RangeX, RangeY;
    ULONG   Expect, Seen;
    int     PrevN;
    ULONG   PrevId;
    LONG    PrevX, PrevY, RemX, RemY;
    LONG    PrevCx, PrevCy, AccX, AccY;
    ULONG   TapStart, TapUntil;
    LONG    TapX0, TapY0, TapMove;
    LONG    SwX0, SwY0;                 /* where a three finger swipe started */
    PTP_OUT Out;
    void   *Ctx;
} PTP_STATE;

/* HID Precision Touchpads */
void    PtpInit(PTP_STATE *S, const HID_PTP_LAYOUT *L);
ULONG   PtpModeReport(const HID_PTP_LAYOUT *L, UCHAR *Out, ULONG Max);
void    PtpReport(PTP_STATE *S, const UCHAR *Rep, ULONG Len, void *Mouse);

/* every other touch pad: contacts decoded by the caller */
void    PtpInitPad(PTP_STATE *S, LONG MinX, LONG MaxX, LONG MinY, LONG MaxY, ULONG Flags, PTP_OUT Out, void *Ctx);
void    PtpFrame(PTP_STATE *S, const PTP_TOUCH *T, ULONG N, UCHAR Buttons);

/* both: Mouse = the HidExtAdd handle of PtpReport, NULL for PtpInitPad states */
void    PtpTick(PTP_STATE *S, void *Mouse);
BOOLEAN PtpBusy(const PTP_STATE *S);

#endif
