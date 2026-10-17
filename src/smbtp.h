/*
 * smbtp.h - SMBus host controllers (smbus.c) and touch pads on SMBus
 * (smbtp.c, smbtpdec.c): Synaptics RMI4 "InterTouch" and Elan.
 */
#ifndef SMBTP_H
#define SMBTP_H

#include "usbnt.h"

/* ------------------------------------------------------------------ */
/* SMBus host controllers (smbus.c)                                     */
/* ------------------------------------------------------------------ */

#define SMB_OK          0
#define SMB_PENDING     0x1000          /* not a byte or a length */
#define SMB_BUSY        (-30)           /* host or semaphore taken by the firmware / another device */
#define SMB_NACK        (-31)           /* no answer from the address */
#define SMB_ERR         (-32)           /* bus error, collision, failed transaction */
#define SMB_TIMEOUT     (-33)
#define SMB_PROTO       (-34)           /* bad block length */

#define SMB_BLOCK_MAX   32

typedef struct _SMB_HOST SMB_HOST;

extern int SmbHostCount;

void      SmbHostProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id, ULONG Class);
SMB_HOST *SmbHostGet(int Index);
int       SmbHostPorts(SMB_HOST *H);    /* multiplexed ports (AMD FCH), 1 for Intel */
const char *SmbHostName(SMB_HOST *H);
int       SmbHostDescribe(SMB_HOST *H, char *Buf, int Size);

/* synchronous transfers (worker context); a block read returns its length */
int SmbReadByteData(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd);
int SmbWriteByte(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd);
int SmbReadBlock(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd, UCHAR *Buf);
int SmbWriteBlock(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd, const UCHAR *Buf, ULONG Len);
/* asynchronous block read for polling: start it, then collect it on later passes */
int SmbStartReadBlock(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd);
int SmbCollect(SMB_HOST *H, UCHAR *Buf);   /* SMB_PENDING, the length or an error */
BOOLEAN SmbHostIdle(SMB_HOST *H);

/* ------------------------------------------------------------------ */
/* PS/2 side of the touch pad (the identification that decides)         */
/* ------------------------------------------------------------------ */

/* aux (mouse) port access given by the PS/2 driver */
typedef struct _SMB_PS2_IO {
    void    *Ctx;
    BOOLEAN (*Send)(void *Ctx, UCHAR Byte);     /* to the aux device; TRUE when it answered FA */
    int     (*Read)(void *Ctx, ULONG Ms);       /* next byte from the aux device, -1 after Ms */
} SMB_PS2_IO;

#define SMB_PAD_NONE        0
#define SMB_PAD_SYNAPTICS   1           /* RMI4 over SMBus, address 0x2C */
#define SMB_PAD_ELAN        2           /* Elan over SMBus, address 0x15 */

/* what the PS/2 identification found (smbtpdec.c) */
typedef struct _SMB_PS2_INFO {
    UCHAR   Kind;                       /* SMB_PAD_* of the pad on the PS/2 port */
    UCHAR   SmbusCapable;               /* the pad says it has an SMBus side */
    UCHAR   ClickPad;
    UCHAR   StickButtons;               /* Synaptics: trackpoint buttons are the pad's GPIOs 3-5 */
    UCHAR   ForcePad;
    UCHAR   Pad[3];
    ULONG   Identity, Caps, ExtCap, ExtCap0c, ExtCap10, BoardId;    /* Synaptics */
    ULONG   FwVersion;                  /* Elan */
    UCHAR   HwVersion, Bus, NewIcHostNotify, Pad2;
} SMB_PS2_INFO;

void    SmbPs2Identify(const SMB_PS2_IO *Io, SMB_PS2_INFO *Out);

/* Called by the PS/2 mouse driver right after the mouse reset (FF, AA 00), at
   PASSIVE_LEVEL, before it enables the mouse.  Identifies the pad; when it is a
   Synaptics InterTouch / Elan SMBus pad that should be used on SMBus (registry
   Smbus, whitelist) and the SMBus side answers, the PS/2 side is left disabled
   and TRUE is returned: the PS/2 driver must then not use the mouse (no F4, ignore
   its bytes).  FALSE: the mouse is a PS/2 device as before. */
BOOLEAN SmbusTouchpadTakeOver(const SMB_PS2_IO *Io);

/* ------------------------------------------------------------------ */
/* decoders (smbtpdec.c, also built into the host unit test)            */
/* ------------------------------------------------------------------ */

#define SMB_MAX_CONTACTS    5

typedef struct _SMB_CONTACT {
    UCHAR   Down;
    UCHAR   Id;
    UCHAR   Pad[2];
    LONG    X, Y;                       /* 0..MaxX / 0..MaxY, Y growing downwards */
} SMB_CONTACT;

typedef struct _SMB_FRAME {
    int         N;                      /* contacts down */
    SMB_CONTACT C[SMB_MAX_CONTACTS];
    UCHAR       PadButton;              /* click pad pressed */
    UCHAR       Buttons;                /* separate buttons: bit 0 left, 1 right, 2 middle */
} SMB_FRAME;

/* RMI4 register descriptor (F12): the registers present with their sizes */
#define RMI_DESC_REGS       64
typedef struct _RMI_REG_DESC {
    UCHAR   Present[RMI_DESC_REGS / 8];
    USHORT  Size[RMI_DESC_REGS];
    UCHAR   Subpackets[RMI_DESC_REGS];
    ULONG   SubMap[RMI_DESC_REGS];      /* the first 32 subpackets */
} RMI_REG_DESC;

int     RmiParseRegDesc(const UCHAR *Presence, ULONG PresenceLen, const UCHAR *Struct, ULONG StructLen,
                        RMI_REG_DESC *D);
ULONG   RmiDescStructSize(const UCHAR *Presence, ULONG PresenceLen);
BOOLEAN RmiDescHas(const RMI_REG_DESC *D, ULONG Reg);
int     RmiDescIndex(const RMI_REG_DESC *D, ULONG Reg);         /* address offset of a register, -1 */
ULONG   RmiDescByteOffset(const RMI_REG_DESC *D, ULONG Reg);    /* offset in a packet read */

/* F12 data 1 (8 bytes per object) and F11 (finger state + 5 bytes per finger);
   FlipY: InterTouch pads count Y upwards */
void    RmiF12Decode(const UCHAR *Data1, ULONG Len, int Objects, LONG MaxX, LONG MaxY, BOOLEAN FlipY, SMB_FRAME *F);
void    RmiF11Decode(const UCHAR *Data, ULONG Len, int Fingers, LONG MaxX, LONG MaxY, BOOLEAN FlipY, SMB_FRAME *F);
/* F30 / F3A: GPIO bits are low while pressed; Map[i] 0 none, 1-3 button (left, right, middle),
   0x81-0x83 trackpoint button */
UCHAR   RmiGpioButtons(const UCHAR *Data, ULONG Count, const UCHAR *Map, UCHAR *Stick);
/* F03 (PS/2 pass-through): the bytes from the output buffers */
int     RmiF03Bytes(const UCHAR *Obs, ULONG Len, UCHAR *Out);

/* Elan SMBus report (the block read of command A8, report id first) */
#define ELAN_REP_TOUCH      1
#define ELAN_REP_STICK      2
int     ElanDecode(const UCHAR *Rep, ULONG Len, LONG MaxX, LONG MaxY, SMB_FRAME *F, LONG *Dx, LONG *Dy,
                   UCHAR *StickButtons);

/* standard 3 byte PS/2 mouse packets (trackpoint behind F03) */
typedef struct _SMB_PS2_PKT {
    UCHAR   B[3];
    UCHAR   N;
} SMB_PS2_PKT;
BOOLEAN SmbPs2Packet(SMB_PS2_PKT *P, UCHAR Byte, LONG *Dx, LONG *Dy, UCHAR *Buttons);

/* Linux's InterTouch PNP id list (synaptics.c smbus_pnp_ids + topbuttonpad_pnp_ids) */
BOOLEAN SmbPnpWhitelisted(const char *Id);
BOOLEAN SmbPnpForcePad(const char *Id);

/* ------------------------------------------------------------------ */
/* the touch pads (smbtp.c)                                             */
/* ------------------------------------------------------------------ */

extern int SmbTpCount;
void    SmbusInit(void);                /* finds the host controllers (PCI scan of UsbInit) */
void    SmbusService(void);             /* worker: polling */
void    SmbusShutdown(void);
int     SmbusDescribe(int Index, char *Buf, int Size);
BOOLEAN SmbusInfo(int Index, UCHAR *Kind, char *Name, int NameSize, char *Detail, int DetailSize);

#endif
