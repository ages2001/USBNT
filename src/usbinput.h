/*
 * usbinput.h - keyboard and mouse events delivered by the HID driver.
 * Flag values match the NT KEYBOARD_INPUT_DATA / MOUSE_INPUT_DATA ones.
 */

#ifndef USBINPUT_H
#define USBINPUT_H

#include "usbnt.h"

#define UKEY_MAKE           0x0000
#define UKEY_BREAK          0x0001
#define UKEY_E0             0x0002
#define UKEY_E1             0x0004

#define UMOU_LEFT_DOWN      0x0001
#define UMOU_LEFT_UP        0x0002
#define UMOU_RIGHT_DOWN     0x0004
#define UMOU_RIGHT_UP       0x0008
#define UMOU_MIDDLE_DOWN    0x0010
#define UMOU_MIDDLE_UP      0x0020
#define UMOU_B4_DOWN        0x0040
#define UMOU_B4_UP          0x0080
#define UMOU_B5_DOWN        0x0100
#define UMOU_B5_UP          0x0200
#define UMOU_WHEEL          0x0400

/* NT keyboard indicator bits */
#define ULED_SCROLL         0x0001
#define ULED_NUM            0x0002
#define ULED_CAPS           0x0004

typedef struct _USB_KEY_EVENT {
    USHORT MakeCode;
    USHORT Flags;
} USB_KEY_EVENT;

typedef struct _USB_MOUSE_EVENT {
    USHORT ButtonFlags;
    SHORT  Wheel;           /* detents, positive = away from the user */
    SHORT  Pan;
    UCHAR  Absolute;
    UCHAR  Pad;
    LONG   X;               /* relative counts, or 0..65535 when absolute */
    LONG   Y;
} USB_MOUSE_EVENT;

/* Implemented by the OS layer, called at DPC level */
void    OsKbdInput(const USB_KEY_EVENT *Events, ULONG Count);
void    OsMouseInput(const USB_MOUSE_EVENT *Event);

/* Called by the OS layer */
void    HidSetLeds(ULONG NtIndicators);
void    HidSetTypematic(ULONG DelayMs, ULONG RateCps);
ULONG   HidKeyboardCount(void);
ULONG   HidMouseCount(void);
ULONG   HidMouseButtons(void);
BOOLEAN HidMouseHasWheel(void);

/* Report descriptor parser (hidparse.c) */
typedef struct _HID_FIELD {
    UCHAR   Valid;
    UCHAR   ReportId;
    UCHAR   Size;
    UCHAR   Signed;
    UCHAR   Relative;
    UCHAR   Count;
    USHORT  Offset;
    LONG    Min;
    LONG    Max;
} HID_FIELD;

typedef struct _HID_MOUSE_LAYOUT {
    UCHAR       UsesIds;
    UCHAR       ReportId;
    USHORT      ReportBits;
    HID_FIELD   Buttons;
    HID_FIELD   X;
    HID_FIELD   Y;
    HID_FIELD   Wheel;
    HID_FIELD   Pan;
} HID_MOUSE_LAYOUT;

int     HidParseMouse(const UCHAR *Desc, ULONG Len, HID_MOUSE_LAYOUT *Out);
LONG    HidGetField(const UCHAR *Report, ULONG Len, const HID_FIELD *F, ULONG Index);

#endif
