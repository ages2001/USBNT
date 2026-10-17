/*
 * usbntaud.h - USB audio interface of USBNT for other kernel drivers
 * (the AUDIONT sound driver). Self contained: needs only the NT basic
 * types (ULONG, USHORT, UCHAR, SHORT, BOOLEAN).
 *
 A client opens \Device\UsbNtAudio with IoGetDeviceObjectPointer and
 * either sends IOCTL_USBNT_AUDIO_INTERFACE as an internal device control
 * request (METHOD_NEITHER, output buffer = USBNT_AUDIO_IF) or reads the
 * table from the device extension (USBNT_AUDIO_EXT, check Magic and
 * If->Version). The functions are then called directly:
 *
 *   Register / GetInfo / Open / Close       PASSIVE_LEVEL
 *   Start / Stop / SetVolume / SelectInput /
 *   GetStats / MidiOut                       <= DISPATCH_LEVEL
 *   UA_CLIENT callbacks                      USBNT worker thread (PASSIVE_LEVEL)
 *   UA_STREAM_OPS callbacks                  DISPATCH_LEVEL
 */

#ifndef USBNTAUD_H
#define USBNTAUD_H

#define USBNT_AUDIO_DEVICE_NAME     L"\\Device\\UsbNtAudio"
/* CTL_CODE(FILE_DEVICE_UNKNOWN, 0x910, METHOD_NEITHER, FILE_ANY_ACCESS) */
#define IOCTL_USBNT_AUDIO_INTERFACE 0x00222443
#define USBNT_AUDIO_IF_VERSION      1

/* every function and callback of the interface is __cdecl, whatever the
   compiler's default (the NT DDK builds with __stdcall, /Gz) */
#ifndef UA_CALL
#if defined(_MSC_VER) || defined(_WIN32) || defined(__MINGW32__)
#define UA_CALL __cdecl
#elif defined(__GNUC__) && defined(__i386__)
#define UA_CALL __attribute__((cdecl))
#else
#define UA_CALL
#endif
#endif

#define UA_MAX_DEVS         8
#define UA_MAX_FORMATS      16
#define UA_MAX_RATES        16
#define UA_MAX_INPUTS       8

#define UA_PLAY             0
#define UA_REC              1

#define UA_FMT_PCM          0       /* signed little endian */
#define UA_FMT_PCM8         1       /* unsigned 8 bit */
#define UA_FMT_FLOAT        2       /* IEEE 754 32 bit */

typedef struct _UA_FORMAT {
    UCHAR   Alt;                    /* alternate setting of the streaming interface */
    UCHAR   Type;                   /* UA_FMT_* */
    UCHAR   Channels;
    UCHAR   SubSize;                /* bytes per sample in the stream */
    UCHAR   Bits;                   /* valid bits */
    UCHAR   NumRates;               /* 0 = continuous RateMin..RateMax */
    UCHAR   Sync;                   /* 1 async, 2 adaptive, 3 synchronous */
    UCHAR   Feedback;               /* explicit feedback endpoint present */
    ULONG   RateMin, RateMax;
    ULONG   Rates[UA_MAX_RATES];
} UA_FORMAT;

typedef struct _UA_INFO {
    ULONG   Id;                     /* handle, never 0 */
    USHORT  Vid, Pid;
    UCHAR   Uac;                    /* 1 or 2 */
    UCHAR   Speed;
    UCHAR   HasDir[2];
    UCHAR   NumFormats[2];
    UCHAR   HasVolume[2];
    UCHAR   HasMute[2];
    SHORT   VolMin[2], VolMax[2], VolRes[2];        /* 1/256 dB */
    UCHAR   NumInputs;              /* capture sources behind a selector */
    UCHAR   MidiOutCables;          /* USB MIDI */
    UCHAR   MidiInCables;
    UCHAR   Pad;
    USHORT  InputType[UA_MAX_INPUTS];               /* terminal types */
    char    Name[48];
    UA_FORMAT Formats[2][UA_MAX_FORMATS];
} UA_INFO;

typedef struct _UA_STREAM_INFO {
    ULONG   Rate;
    UCHAR   Type;
    UCHAR   Channels;
    UCHAR   SubSize;
    UCHAR   Bits;
    UCHAR   Alt;
    UCHAR   Pad[3];
    ULONG   LatencyMs;              /* data queued ahead of the device */
} UA_STREAM_INFO;

typedef struct _UA_STATS {
    ULONG   Packets;
    ULONG   Frames;
    ULONG   Late;                   /* schedule ran dry */
    ULONG   Errors;                 /* packets with an error status */
    ULONG   Feedback;               /* last feedback, Q16.16 frames per packet */
    ULONG   FeedbackCount;
    ULONG   Running;
    ULONG   MaxGapMs;               /* longest time between two completions while running */
    ULONG   Gaps;                   /* completions more than 30 ms apart */
    ULONG   TickMaxGapMs;           /* longest time between two USBNT poll ticks */
} UA_STATS;

/* stream callbacks, called at dispatch level */
typedef struct _UA_STREAM_OPS {
    void   *Context;
    void  (UA_CALL *Fill)(void *Ctx, UCHAR *Buf, ULONG Frames);         /* playback */
    void  (UA_CALL *Data)(void *Ctx, const UCHAR *Buf, ULONG Frames);   /* capture */
} UA_STREAM_OPS;

/* client callbacks, called in the USB worker context */
typedef struct _UA_CLIENT {
    void   *Context;
    void  (UA_CALL *Arrival)(void *Ctx, const UA_INFO *Info);
    void  (UA_CALL *Removal)(void *Ctx, ULONG Id);
} UA_CLIENT;

typedef struct _USBNT_AUDIO_IF {
    ULONG   Version;                /* USBNT_AUDIO_IF_VERSION */
    ULONG   Size;                   /* sizeof(USBNT_AUDIO_IF) */
    int   (UA_CALL *Register)(const UA_CLIENT *Client);
    void  (UA_CALL *Unregister)(void);
    int   (UA_CALL *GetInfo)(ULONG Id, UA_INFO *Info);
    int   (UA_CALL *Open)(ULONG Id, ULONG Dir, ULONG Rate, ULONG Channels, ULONG Bits,
                  const UA_STREAM_OPS *Ops, UA_STREAM_INFO *Out);
    void  (UA_CALL *Close)(ULONG Id, ULONG Dir);
    int   (UA_CALL *Start)(ULONG Id, ULONG Dir);
    void  (UA_CALL *Stop)(ULONG Id, ULONG Dir);
    int   (UA_CALL *SetVolume)(ULONG Id, ULONG Dir, SHORT Left, SHORT Right, BOOLEAN Mute);
    int   (UA_CALL *SelectInput)(ULONG Id, ULONG Index);
    void  (UA_CALL *GetStats)(ULONG Id, ULONG Dir, UA_STATS *Stats);
    int   (UA_CALL *MidiOut)(ULONG Id, ULONG Cable, const UCHAR *Data, ULONG Len);
} USBNT_AUDIO_IF;

/* device extension of \Device\UsbNtAudio */
#define USBNT_AUDIO_EXT_MAGIC       0x55414E55      /* "UNAU" */
typedef struct _USBNT_AUDIO_EXT {
    ULONG   Kind;                   /* USBNT private */
    ULONG   Number;
    void   *Self;
    ULONG   Magic;                  /* USBNT_AUDIO_EXT_MAGIC */
    const USBNT_AUDIO_IF *If;
} USBNT_AUDIO_EXT;

/* return values of the functions */
#define UA_OK               0
#define UA_ERR_NODEV        (-5)
#define UA_ERR_NOMEM        (-6)
#define UA_ERR_PARAM        (-7)

#endif
