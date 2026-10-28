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
void    OsMousePan(LONG Pan);                   /* horizontal wheel only, to usbmon (WM_HSCROLL) */

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

#define HID_PTR_MOUSE       0       /* mouse or pointer collection */
#define HID_PTR_DIGITIZER   1       /* touch screen / pen: absolute, tip switch = left button */
#define HID_PTR_TOUCHPAD    2       /* touch pad without a mouse collection: finger motion is relative */

typedef struct _HID_MOUSE_LAYOUT {
    UCHAR       UsesIds;
    UCHAR       ReportId;
    USHORT      ReportBits;
    UCHAR       Kind;
    UCHAR       ButtonsFirst;   /* button number of the first bit of Buttons (1 = left) */
    UCHAR       Buttons2First;
    UCHAR       Pad;
    HID_FIELD   Buttons;
    HID_FIELD   Buttons2;       /* a second button field (e.g. buttons 4 and 5 after 1-3) */
    HID_FIELD   X;
    HID_FIELD   Y;
    HID_FIELD   Wheel;
    HID_FIELD   Pan;
    HID_FIELD   Tip;
    HID_FIELD   Barrel;
} HID_MOUSE_LAYOUT;

/* keyboard report layout; the offsets that mark a field are stored plus one */
typedef struct _HID_KBD_LAYOUT {
    UCHAR       Found;
    UCHAR       UsesIds;
    UCHAR       ReportId;
    UCHAR       Arr;
    UCHAR       ArrSize;
    UCHAR       ArrCount;
    UCHAR       ArrMin;
    UCHAR       Bmp;
    UCHAR       BmpMin;
    UCHAR       BmpCount;
    UCHAR       LedValid;
    UCHAR       LedId;
    USHORT      ReportBits;
    USHORT      ArrOff;
    USHORT      BmpOff;
    USHORT      LedBits;
    USHORT      ModOff[8];
    USHORT      LedOff[3];
    LONG        ArrLogMin;
} HID_KBD_LAYOUT;

/* Windows Precision Touchpad (multi-touch) report layout and its Input Mode feature */
#define HID_PTP_FINGERS     5
typedef struct _HID_PTP_FINGER {
    HID_FIELD   Tip;
    HID_FIELD   Conf;
    HID_FIELD   Id;
    HID_FIELD   X;
    HID_FIELD   Y;
} HID_PTP_FINGER;

typedef struct _HID_PTP_LAYOUT {
    UCHAR       Valid;
    UCHAR       UsesIds;
    UCHAR       ReportId;
    UCHAR       NFingers;
    HID_PTP_FINGER F[HID_PTP_FINGERS];
    HID_FIELD   Count;          /* contact count */
    HID_FIELD   Button;         /* the pad's button (click pad) */
    UCHAR       ModeValid;      /* Input Mode feature found */
    UCHAR       ModeId;
    UCHAR       ModeSize;
    UCHAR       Pad;
    USHORT      ModeOff;
    USHORT      ModeBits;       /* bits of the whole feature report */
} HID_PTP_LAYOUT;

int     HidParsePtp(const UCHAR *Desc, ULONG Len, HID_PTP_LAYOUT *Out);
int     HidParseMouse(const UCHAR *Desc, ULONG Len, HID_MOUSE_LAYOUT *Out);
int     HidParseKeyboard(const UCHAR *Desc, ULONG Len, HID_KBD_LAYOUT *Out);
BOOLEAN HidKbdToBoot(const HID_KBD_LAYOUT *L, const UCHAR *Rep, ULONG Len, UCHAR *Boot);
ULONG   HidKbdLedReport(const HID_KBD_LAYOUT *L, UCHAR HidLeds, UCHAR *Out, ULONG Max);
LONG    HidGetField(const UCHAR *Report, ULONG Len, const HID_FIELD *F, ULONG Index);

/* keyboards and pointers that are not USB devices (HID over I2C): reports
   go through the same code as the USB ones (usbhid.c) */
#define HID_EXT_KBD         1
#define HID_EXT_MOUSE       2
typedef void (*HID_LED_FN)(void *Ctx, UCHAR HidLeds);
void   *HidExtAdd(UCHAR Kind, const HID_MOUSE_LAYOUT *Layout, HID_LED_FN LedFn, void *LedCtx);
void    HidExtReport(void *Handle, const UCHAR *Rep, ULONG Len);
/* a pointer event worked out by the caller (touch pad gestures): buttons bit 0 left,
   1 right, 2 middle, 3 and 4 buttons 4 and 5 (back, forward); motion in mouse counts, wheel / pan in detents */
void    HidExtMouse(void *Handle, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan);
void    HidExtRemove(void *Handle);

#endif
