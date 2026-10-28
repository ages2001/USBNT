/*
 * tbt.c - Thunderbolt 1 to 5 and USB4 (v1, v2) host routers.
 *
 * A Thunderbolt or USB4 controller carries PCIe, USB 3 and DisplayPort over
 * one cable.  Its host interface (NHI) is a DMA engine; ring 0 of it is the
 * control channel to the routers (the host router itself, docks, enclosures).
 * Somebody has to set up the tunnels through them, the connection manager:
 *
 *  - Intel Thunderbolt 3 controllers (Falcon, Alpine, Titan, Maple Ridge) and
 *    the Thunderbolt 4 / USB4 hosts of Ice Lake to Meteor Lake have one in
 *    their firmware (ICM).  It waits for the driver to say "ready", reports
 *    every device that is plugged in, and connects it when told so: plain
 *    approval (security level "user"), or with a 256 bit key that the device
 *    keeps and that is checked by a challenge on later connections (level
 *    "secure").  Keys are kept in the registry.
 *
 *  - Other USB4 hosts (AMD, Intel Lunar Lake, Panther Lake, Wildcat Lake,
 *    the Thunderbolt 5 controllers (Barlow Ridge, USB4 v2), Meteor Lake when
 *    the firmware leaves it to the OS, and others) need the driver to be the
 *    connection manager: enumerate each router through the control channel,
 *    give it a route, program the paths (hop tables) of a PCIe tunnel and a
 *    USB 3 tunnel to it, and react to plug and unplug events.  The same code
 *    drives Thunderbolt 1 / 2 / 3 host routers whose firmware does not do it
 *    (Apple Macs: Light, Cactus, Falcon, Alpine and Titan Ridge).
 *
 *  - Thunderbolt 5 / USB4 v2 links run at 2 x 40 Gb/s (Gen 4, bonded by the
 *    hardware); the routers are told the connection manager knows USB4 v2
 *    and a PCIe tunnel between two of them uses extended encapsulation.
 *
 *  - Thunderbolt 1 / 2 controllers of PCs (Light, Cactus, Redwood, Win
 *    Ridge) are run by the BIOS (SMI "BIOS assist"): the driver leaves them
 *    alone, devices show up through PCIe hot plug.
 *
 * The tunnelled PCIe link shows up at a hot plug capable PCIe port of the
 * host; what is behind it (NVMe drives, the xHCI of a dock, a card reader)
 * is picked up by the PCIe hot plug code of usbcore.c.  USB 3 tunnels end at
 * the xHCI of the host, which is driven like any other.
 *
 * Everything is polled: no interrupt line can be shared with NT drivers that
 * do not know about it.  Control packets are dwords in big endian order with
 * a CRC-32C at the end.
 */

#include "usbnt.h"
#include "tbt.h"

#define TB_HOSTS        2
#define TB_RING         16              /* descriptors in the control rings */
#define TB_FRAME        256             /* control frame size */
#define TB_ROUTERS      10              /* routers per host, the host router included */
#define TB_PORTS        64
#define TB_DEVS         8               /* devices reported by the firmware */
#define TB_EVQ          16

/* NHI registers (BAR 0) */
#define NHI_TX_RING     0x00000         /* + 16 * ring */
#define NHI_RX_RING     0x08000
#define NHI_TX_OPT      0x19800         /* + 32 * ring */
#define NHI_RX_OPT      0x29800
#define NHI_INT_BASE    0x38200
#define NHI_CAPS        0x39640
#define NHI_RESET       0x39898
#define NHI_INMAIL_DATA 0x39900
#define NHI_INMAIL_CMD  0x39904
#define NHI_OUTMAIL_CMD 0x3990C
#define NHI_FW_STS      0x39944
#define NHI_LEN         0x40000

#define FW_STS_NVM_AUTH_DONE    0x80000000UL
#define FW_STS_ICM_EN           0x00000001UL
#define INMAIL_OP_REQUEST       0x80000000UL
#define INMAIL_ERROR            0x40000000UL
#define MAIL_ALLOW_ALL_DEVS     0x23
#define FW_MODE_SAFE            0
#define FW_MODE_CM              3

#define RING_ENABLE     0x80000000UL
#define RING_RAW        0x40000000UL
#define DESC_COMPLETED  (0x2UL << 20)
#define DESC_POSTED     (0x4UL << 20)

/* PCI configuration of the Ice Lake and later NHIs */
#define VS_CAP_9        0xC8            /* bit 31: firmware ready */
#define VS_CAP_15       0xE0
#define VS_CAP_16       0xE4
#define VS_CAP_22       0xFC            /* bit 1: force power, 31:24 DMA delay */

/* control packet types (also the start / end of frame codes) */
#define PKG_READ        1
#define PKG_WRITE       2
#define PKG_ERROR       3
#define PKG_NOTIFY_ACK  4
#define PKG_EVENT       5
#define PKG_XDOMAIN_REQ 6
#define PKG_XDOMAIN_RESP 7
#define PKG_OVERRIDE    8
#define PKG_RESET       9
#define PKG_ICM_EVENT   10
#define PKG_ICM_CMD     11
#define PKG_ICM_RESP    12

/* error packet codes */
#define ERR_ACK_PLUG    7
#define ERR_DP_BW       32
#define ERR_PCIE_WAKE   35
#define ERR_DP_CON      36
#define ERR_DPTX        37

/* configuration spaces */
#define SP_HOPS         0
#define SP_PORT         1
#define SP_SWITCH       2

/* router registers (dwords) */
#define CS_5_CNS        0x00800000UL
#define CS_5_PTO        0x01000000UL
#define CS_5_UTO        0x02000000UL
#define CS_5_HCO        0x04000000UL
#define CS_5_CV         0x80000000UL
#define CS_6_TNS        0x00000002UL
#define CS_6_HCI        0x00040000UL
#define CS_6_RR         0x01000000UL
#define CS_6_CR         0x02000000UL
#define CS_4_CMUV_V1    0x10            /* connection manager USB4 version (15:8) */
#define CS_4_CMUV_V2    0x20

/* adapter types */
#define PT_LANE         0x000001UL
#define PT_NHI          0x000002UL
#define PT_PCIE_DOWN    0x100101UL
#define PT_PCIE_UP      0x100102UL
#define PT_USB3_DOWN    0x200101UL
#define PT_USB3_UP      0x200102UL

/* adapter registers */
#define ADP_CS_4        4
#define ADP_CS_4_LCK    0x80000000UL
#define ADP_CS_5        5
#define ADP_CS_5_DHP    0x80000000UL
#define CAP_PHY         0x01
#define CAP_ADAP        0x04
#define CAP_VSE         0x05
#define CAP_USB4        0x06
#define PORT_STATE_UP   2
#define PORT_CS_18      0x12
#define PORT_CS_18_TCM  0x00000200UL
#define PORT_CS_18_BE   0x00000100UL
#define LANE_LB         0x00008000UL
#define PORT_CS_19      0x13
#define PORT_CS_19_PC   0x00000008UL
#define PCIE_PE         0x80000000UL
#define PCIE_CS_1_EE    0x00000001UL    /* ADP_PCIE_CS_1: extended encapsulation (USB4 v2) */
#define USB3_PE         0x80000000UL
#define USB3_V          0x40000000UL
#define USB3_HCA        0x80000000UL
#define USB3_CMR        0x80000000UL

/* Thunderbolt 3 routers: vendor specific capabilities */
#define VSE_PLUG_EVENTS 0x01
#define VSE_LC          0x06
#define LC_DESC         0x02
#define LC_SX_CTRL      0x96
#define LC_PORT_ATTR    0x8D
#define LC_PORT_ATTR_BE 0x00001000UL
#define LC_SX_L1C       0x00010000UL
#define LC_SX_L2C       0x00100000UL
#define LC_SX_UPSTREAM  0x40000000UL

#define HOPID_MIN       8
#define PCIE_HOPID      8
#define USB3_HOPID      8

/* firmware connection manager */
#define ICM_DRIVER_READY        0x03
#define ICM_APPROVE_DEVICE      0x04
#define ICM_CHALLENGE_DEVICE    0x05
#define ICM_ADD_DEVICE_KEY      0x06
#define ICM_EV_CONNECTED        0x03
#define ICM_EV_DISCONNECTED     0x04
#define ICM_FL_ERROR            0x01
#define ICM_FL_NO_KEY           0x02
#define LINK_APPROVED           0x0100
#define LINK_REJECTED           0x0200
#define LINK_BOOT               0x0400

/* generations of the firmware interface */
#define GEN_FR          1               /* Falcon Ridge */
#define GEN_AR          2               /* Alpine Ridge */
#define GEN_TR          3               /* Titan Ridge, Maple Ridge */
#define GEN_ICL         4               /* Ice Lake to Meteor Lake, no approval by the driver */
#define GEN_USB4        5               /* driver is the connection manager */
#define GEN_LEGACY      6               /* Thunderbolt 1 / 2 without firmware connection manager */
#define GEN_USBONLY     7               /* Alpine Ridge in USB only mode: nothing to manage */

static const char *SlName[] = { "none", "user", "secure", "dponly", "usbonly", "nopcie" };

typedef struct _TB_DESC {
    ULONG Lo, Hi, Ctl, Time;
} TB_DESC;

typedef struct _TB_PKT {
    UCHAR Type;
    UCHAR Dw;                           /* without the CRC */
    ULONG D[63];
} TB_PKT;

typedef struct _TB_PORT {
    ULONG Type;
    UCHAR CapPhy, CapUsb4, CapAdap;
    BOOLEAN Lane1;                      /* second lane of a USB4 port */
    BOOLEAN Bonded;                     /* both lanes work as one link */
    UCHAR Child;                        /* router index + 1 behind this lane */
    ULONG MaxInHop;
    ULONGLONG HopIn, HopOut;            /* link HopIDs in use (bit = HopID) */
} TB_PORT;

typedef struct _TB_ROUTER {
    BOOLEAN Used;
    UCHAR Parent;                       /* index + 1 (0: host router) */
    UCHAR ParentPort;                   /* lane adapter of the parent */
    UCHAR Depth, Up, MaxPort;
    UCHAR Version;                      /* USB4 major version, 0 = Thunderbolt 1 / 2 / 3 */
    BOOLEAN LinkUsb4;
    UCHAR LinkGbps, LinkLanes;          /* upstream link: per lane speed, lanes */
    ULONG Route;                        /* depth <= 4 */
    ULONG Id;
    UCHAR PcieDown, PcieUp, PcieLD, PcieLU;
    BOOLEAN PcieEe;                     /* PCIe tunnel with extended encapsulation */
    BOOLEAN HwBonded;                   /* Gen 4 link: bonded by the hardware, stays so */
    UCHAR Usb3Down, Usb3Up, Usb3LD, Usb3LU;
    TB_PORT P[TB_PORTS];
} TB_ROUTER;

typedef struct _TB_DEV {
    BOOLEAN Used;
    UCHAR Uuid[16];
    UCHAR ConnKey, ConnId;
    ULONG RouteHi, RouteLo;
    USHORT LinkInfo;
    UCHAR Sl;
    UCHAR Gbps;                         /* link speed the firmware reports (lanes x speed) */
    UCHAR State;                        /* DS_ */
    USHORT Vendor, Model;
    UCHAR Key[32];
    UCHAR Challenge[32];
    BOOLEAN HaveKey, NewKey;
    char Note[40];
} TB_DEV;

#define DS_NEW          0
#define DS_WAIT         1
#define DS_APPROVE      2
#define DS_ADDKEY       3
#define DS_CHALLENGE    4
#define DS_DONE         5
#define DS_REFUSED      6

#define HS_START        0
#define HS_POWER        1
#define HS_DRVREADY     2
#define HS_ROOT         3
#define HS_ICM          4
#define HS_SW           5
#define HS_DEAD         6
#define HS_BIOS         7               /* the BIOS (or nobody) manages it */

typedef struct _TB_HOST {
    BOOLEAN Used;
    UCHAR Bus, Dev, Fn;
    USHORT DevId;
    ULONG Cls;
    UCHAR Gen;
    UCHAR State;
    ULONG Phys;
    volatile UCHAR *Regs;
    ULONG HopCount;
    ULONG StateAt;
    int Tries;
    UCHAR Sl;
    volatile TB_DESC *Tx, *Rx;
    ULONG DescPhys;
    UCHAR *TxBuf, *RxBuf;
    ULONG TxBufPhys, RxBufPhys;
    USHORT TxHead, TxTail, RxHead, RxTail;
    UCHAR Seq;
    /* the request that waits for its answer */
    BOOLEAN Waiting, Done;
    UCHAR WType, WSeq;                  /* PKG_READ / PKG_WRITE: route and sequence; PKG_ICM_RESP: code */
    ULONG WRouteHi, WRouteLo;
    TB_PKT Resp;
    /* firmware request in flight (asynchronous) */
    BOOLEAN IcmBusy, IcmDone;
    TB_PKT IcmResp;
    UCHAR IcmCode;
    int IcmDev;
    ULONG IcmSent, IcmTimeout;
    int IcmTries;
    ULONG IcmReq[64];
    int IcmReqDw;
    /* events */
    TB_PKT Ev[TB_EVQ];
    int EvHead, EvCount;
    TB_DEV D[TB_DEVS];
    TB_ROUTER R[TB_ROUTERS];
    char Name[48];
} TB_HOST;

static TB_HOST *Hosts[TB_HOSTS];
int TbtHostCount;

/* ------------------------------------------------------------------ */
/* CRC-32C, SHA-256, HMAC-SHA-256                                       */
/* ------------------------------------------------------------------ */

static ULONG Crc32c(const UCHAR *P, ULONG N)
{
    ULONG c = 0xFFFFFFFFUL;
    int k;

    while (N--) {
        c ^= *P++;
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0x82F63B78UL & (0UL - (c & 1)));
        }
    }
    return ~c;
}

static const ULONG ShaK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

typedef struct _SHA_CTX {
    ULONG H[8];
    UCHAR Buf[64];
    ULONG Len;                          /* bytes so far (< 512 MB) */
} SHA_CTX;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void ShaBlock(SHA_CTX *C, const UCHAR *B)
{
    ULONG w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((ULONG)B[4 * i] << 24) | ((ULONG)B[4 * i + 1] << 16) | ((ULONG)B[4 * i + 2] << 8) | B[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        ULONG s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        ULONG s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = C->H[0]; b = C->H[1]; c = C->H[2]; d = C->H[3];
    e = C->H[4]; f = C->H[5]; g = C->H[6]; h = C->H[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + ShaK[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    C->H[0] += a; C->H[1] += b; C->H[2] += c; C->H[3] += d;
    C->H[4] += e; C->H[5] += f; C->H[6] += g; C->H[7] += h;
}

static void ShaInit(SHA_CTX *C)
{
    static const ULONG iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    UsbMemCpy(C->H, iv, sizeof(iv));
    C->Len = 0;
}

static void ShaAdd(SHA_CTX *C, const UCHAR *P, ULONG N)
{
    while (N--) {
        C->Buf[C->Len & 63] = *P++;
        C->Len++;
        if ((C->Len & 63) == 0) {
            ShaBlock(C, C->Buf);
        }
    }
}

static void ShaEnd(SHA_CTX *C, UCHAR *Out)
{
    ULONG bits = C->Len << 3, hi = C->Len >> 29;
    UCHAR pad = 0x80, zero = 0, len[8];
    int i;

    ShaAdd(C, &pad, 1);
    while ((C->Len & 63) != 56) {
        ShaAdd(C, &zero, 1);
    }
    PUTBE32(len, hi);
    PUTBE32(len + 4, bits);
    ShaAdd(C, len, 8);
    for (i = 0; i < 8; i++) {
        PUTBE32(Out + 4 * i, C->H[i]);
    }
}

void Sha256(const UCHAR *Data, ULONG Len, UCHAR *Out)
{
    SHA_CTX c;

    ShaInit(&c);
    ShaAdd(&c, Data, Len);
    ShaEnd(&c, Out);
}

void HmacSha256(const UCHAR *Key, ULONG KeyLen, const UCHAR *Data, ULONG Len, UCHAR *Out)
{
    UCHAR k[64], pad[64], inner[32];
    SHA_CTX c;
    int i;

    UsbMemSet(k, 0, sizeof(k));
    if (KeyLen > 64) {
        Sha256(Key, KeyLen, k);
    } else {
        UsbMemCpy(k, Key, KeyLen);
    }
    for (i = 0; i < 64; i++) {
        pad[i] = (UCHAR)(k[i] ^ 0x36);
    }
    ShaInit(&c);
    ShaAdd(&c, pad, 64);
    ShaAdd(&c, Data, Len);
    ShaEnd(&c, inner);
    for (i = 0; i < 64; i++) {
        pad[i] = (UCHAR)(k[i] ^ 0x5C);
    }
    ShaInit(&c);
    ShaAdd(&c, pad, 64);
    ShaAdd(&c, inner, 32);
    ShaEnd(&c, Out);
}

/* random bytes for keys and challenges: timer noise through SHA-256 */
static void Random(UCHAR *Out, ULONG Len)
{
    static UCHAR pool[32];
    static ULONG counter;
    UCHAR seed[32 + 8 + 4 + 4];
    ULONGLONG e;

    while (Len > 0) {
        ULONG n = Len < 32 ? Len : 32;
        e = OsEntropy();
        UsbMemCpy(seed, pool, 32);
        UsbMemCpy(seed + 32, &e, 8);
        counter++;
        UsbMemCpy(seed + 40, &counter, 4);
        e = OsTimeMs();
        UsbMemCpy(seed + 44, &e, 4);
        Sha256(seed, sizeof(seed), pool);
        UsbMemCpy(Out, pool, n);
        seed[0] ^= 0xA5;
        Sha256(seed, sizeof(seed), pool);
        Out += n;
        Len -= n;
    }
}

/* ------------------------------------------------------------------ */
/* NHI and control channel                                              */
/* ------------------------------------------------------------------ */

static ULONG Rd(TB_HOST *h, ULONG Off)
{
    return MmioRd32(h->Regs + Off);
}

static void Wr(TB_HOST *h, ULONG Off, ULONG V)
{
    MmioWr32(h->Regs + Off, V);
}

static BOOLEAN HostGone(TB_HOST *h)
{
    return Rd(h, NHI_FW_STS) == 0xFFFFFFFFUL && Rd(h, NHI_CAPS) == 0xFFFFFFFFUL;
}

static void RingsStop(TB_HOST *h)
{
    Wr(h, NHI_TX_OPT, 0);
    Wr(h, NHI_RX_OPT, 0);
    Wr(h, NHI_TX_RING + 0, 0);
    Wr(h, NHI_TX_RING + 4, 0);
    Wr(h, NHI_TX_RING + 8, 0);
    Wr(h, NHI_TX_RING + 12, 0);
    Wr(h, NHI_RX_RING + 0, 0);
    Wr(h, NHI_RX_RING + 4, 0);
    Wr(h, NHI_RX_RING + 8, 0);
    Wr(h, NHI_RX_RING + 12, 0);
}

static void RingsStart(TB_HOST *h)
{
    ULONG i;

    RingsStop(h);
    /* no interrupts: everything is polled */
    for (i = 0; i < (31 + 2 * h->HopCount) / 32; i++) {
        Wr(h, NHI_INT_BASE + 4 * i, 0);
    }
    UsbMemSet((void *)h->Tx, 0, 2 * TB_RING * sizeof(TB_DESC));
    h->TxHead = h->TxTail = 0;
    h->RxTail = 0;
    Wr(h, NHI_TX_RING + 0, h->DescPhys);
    Wr(h, NHI_TX_RING + 4, 0);
    Wr(h, NHI_TX_RING + 12, TB_RING);
    Wr(h, NHI_TX_OPT + 4, 0);
    Wr(h, NHI_TX_OPT + 0, RING_ENABLE | RING_RAW);

    Wr(h, NHI_RX_RING + 0, h->DescPhys + TB_RING * sizeof(TB_DESC));
    Wr(h, NHI_RX_RING + 4, 0);
    Wr(h, NHI_RX_RING + 12, ((ULONG)TB_FRAME << 16) | TB_RING);
    Wr(h, NHI_RX_OPT + 4, 0xFFFFFFFFUL);        /* every start / end of frame code */
    Wr(h, NHI_RX_OPT + 0, RING_ENABLE | RING_RAW);
    for (i = 0; i < TB_RING - 1; i++) {
        h->Rx[i].Lo = h->RxBufPhys + i * TB_FRAME;
        h->Rx[i].Hi = 0;
        h->Rx[i].Time = 0;
        h->Rx[i].Ctl = DESC_POSTED;
    }
    h->RxHead = TB_RING - 1;
    Wr(h, NHI_RX_RING + 8, h->RxHead);
}

static BOOLEAN Tx(TB_HOST *h, UCHAR Type, const ULONG *D, int Dw)
{
    ULONG start = OsTimeMs();
    USHORT slot;
    UCHAR *b;
    int i;

    for (;;) {
        while (h->TxTail != h->TxHead && (h->Tx[h->TxTail].Ctl & DESC_COMPLETED)) {
            h->TxTail = (USHORT)((h->TxTail + 1) % TB_RING);
        }
        if ((h->TxHead + 1) % TB_RING != h->TxTail) {
            break;
        }
        if (OsTimeMs() - start > 100) {
            UsbLog(LOG_ERR, "%s: control channel stuck (transmit ring full)\n", h->Name);
            return FALSE;
        }
        OsStallUs(50);
    }
    slot = h->TxHead;
    b = h->TxBuf + slot * TB_FRAME;
    for (i = 0; i < Dw; i++) {
        PUTBE32(b + 4 * i, D[i]);
    }
    PUTBE32(b + 4 * Dw, Crc32c(b, (ULONG)Dw * 4));
    h->Tx[slot].Lo = h->TxBufPhys + slot * TB_FRAME;
    h->Tx[slot].Hi = 0;
    h->Tx[slot].Time = 0;
    h->Tx[slot].Ctl = ((ULONG)Dw * 4 + 4) | ((ULONG)Type << 12) | ((ULONG)Type << 16) | DESC_POSTED;
    h->TxHead = (USHORT)((slot + 1) % TB_RING);
    Wr(h, NHI_TX_RING + 8, (ULONG)h->TxHead << 16);
    return TRUE;
}

/* one received frame, FALSE when there is none; P->Type is 0 for a bad frame */
static BOOLEAN Rx(TB_HOST *h, TB_PKT *P)
{
    ULONG ctl = h->Rx[h->RxTail].Ctl;
    ULONG size, i;
    UCHAR *b;

    if (!(ctl & DESC_COMPLETED)) {
        return FALSE;
    }
    size = ctl & 0xFFF;
    b = h->RxBuf + h->RxTail * TB_FRAME;
    P->Type = 0;
    if (size >= 4 && (size & 3) == 0 && size <= TB_FRAME) {
        UCHAR type = (UCHAR)((ctl >> 12) & 0xF);
        BOOLEAN crc = type != PKG_ICM_EVENT && type != PKG_ICM_RESP && type != PKG_ICM_CMD;
        P->Dw = (UCHAR)((size - 4) / 4);
        for (i = 0; i < P->Dw; i++) {
            P->D[i] = GETBE32(b + 4 * i);
        }
        if (!crc || GETBE32(b + size - 4) == Crc32c(b, size - 4)) {
            P->Type = type;
        } else {
            UsbLog(LOG_ERR, "%s: control frame with a bad checksum dropped\n", h->Name);
        }
    }
    /* the free slot gets a buffer again */
    h->Rx[h->RxHead].Lo = h->RxBufPhys + h->RxHead * TB_FRAME;
    h->Rx[h->RxHead].Hi = 0;
    h->Rx[h->RxHead].Time = 0;
    h->Rx[h->RxHead].Ctl = DESC_POSTED;
    h->RxHead = (USHORT)((h->RxHead + 1) % TB_RING);
    h->RxTail = (USHORT)((h->RxTail + 1) % TB_RING);
    Wr(h, NHI_RX_RING + 8, h->RxHead);
    return TRUE;
}

static void Queue(TB_HOST *h, const TB_PKT *P)
{
    if (h->EvCount >= TB_EVQ) {
        UsbLog(LOG_ERR, "%s: too many events, one dropped\n", h->Name);
        return;
    }
    UsbMemCpy(&h->Ev[(h->EvHead + h->EvCount) % TB_EVQ], P, sizeof(*P));
    h->EvCount++;
}

static void Ack(TB_HOST *h, ULONG RouteHi, ULONG RouteLo, ULONG Error, ULONG Port, ULONG Pg)
{
    ULONG d[3];

    d[0] = RouteHi & 0x3FFFFF;
    d[1] = RouteLo;
    d[2] = (Error & 0xFF) | ((Port & 0x3F) << 8) | (Pg << 30);
    Tx(h, PKG_ERROR, d, 3);
}

/* everything that came in: answers to the waiting request, events */
static void Poll(TB_HOST *h)
{
    TB_PKT p;
    int n = 0;

    while (n++ < 64 && Rx(h, &p)) {
        switch (p.Type) {
        case PKG_READ:
        case PKG_WRITE:
            if (h->Waiting && p.Type == h->WType && p.Dw >= 3 && (p.D[0] & 0x3FFFFF) == h->WRouteHi &&
                p.D[1] == h->WRouteLo && ((p.D[2] >> 27) & 3) == h->WSeq) {
                UsbMemCpy(&h->Resp, &p, sizeof(p));
                h->Done = TRUE;
                h->Waiting = FALSE;
            }
            break;
        case PKG_ERROR:
            if (p.Dw >= 3) {
                ULONG err = p.D[2] & 0xFF;
                if (err == 1 || err == 12 || err == 13 || (err >= 32 && err <= 39)) {
                    /* notifications from routers */
                    if (err == ERR_DP_BW || err == ERR_PCIE_WAKE || err == ERR_DP_CON || err == ERR_DPTX) {
                        ULONG d[3];
                        d[0] = p.D[0] & 0x3FFFFF;
                        d[1] = p.D[1];
                        d[2] = p.D[2];
                        Tx(h, PKG_NOTIFY_ACK, d, 2);
                    }
                } else if (h->Waiting && (h->WType == PKG_READ || h->WType == PKG_WRITE)) {
                    /* the router that could not pass the request on answers (its own route) */
                    UsbMemCpy(&h->Resp, &p, sizeof(p));
                    h->Done = TRUE;
                    h->Waiting = FALSE;
                }
            }
            break;
        case PKG_EVENT:
            if (p.Dw >= 3) {
                Ack(h, p.D[0], p.D[1], ERR_ACK_PLUG, p.D[2] & 0x3F, (p.D[2] & 0x80000000UL) ? 3 : 2);
                Queue(h, &p);
            }
            break;
        case PKG_ICM_RESP:
            if (h->IcmBusy && !h->IcmDone && p.Dw >= 1 && (p.D[0] & 0xFF) == h->IcmCode) {
                UsbMemCpy(&h->IcmResp, &p, sizeof(p));
                h->IcmDone = TRUE;
            }
            break;
        case PKG_ICM_EVENT:
            if (p.Dw >= 1) {
                Queue(h, &p);
            }
            break;
        default:
            break;
        }
    }
}

/* configuration space access through the control channel: 0, -1 timeout, error code + 1 negated */
static int Cfg(TB_HOST *h, ULONG Route, UCHAR Space, UCHAR Port, ULONG Off, ULONG *Data, int Len, BOOLEAN Write,
               UCHAR *RespPort, ULONG TimeoutMs)
{
    ULONG pkt[3 + 60];
    ULONG start;
    int i;

    if (Len < 1 || Len > 60) {
        return -1;
    }
    h->Seq = (UCHAR)((h->Seq + 1) & 3);
    pkt[0] = 0;
    pkt[1] = Route;
    pkt[2] = (Off & 0x1FFF) | ((ULONG)Len << 13) | ((ULONG)(Port & 0x3F) << 19) | ((ULONG)Space << 25) |
             ((ULONG)h->Seq << 27);
    if (Write) {
        for (i = 0; i < Len; i++) {
            pkt[3 + i] = Data[i];
        }
    }
    h->WType = Write ? PKG_WRITE : PKG_READ;
    h->WRouteHi = 0;
    h->WRouteLo = Route;
    h->WSeq = h->Seq;
    h->Done = FALSE;
    h->Waiting = TRUE;
    if (!Tx(h, h->WType, pkt, Write ? 3 + Len : 3)) {
        h->Waiting = FALSE;
        return -1;
    }
    start = OsTimeMs();
    while (!h->Done) {
        Poll(h);
        if (h->Done) {
            break;
        }
        if (OsTimeMs() - start > TimeoutMs) {
            h->Waiting = FALSE;
            return -1;
        }
        OsStallUs(20);
    }
    if (h->Resp.Type == PKG_ERROR) {
        return -2 - (int)(h->Resp.D[2] & 0xFF);
    }
    if (RespPort != NULL) {
        *RespPort = (UCHAR)((h->Resp.D[2] >> 19) & 0x3F);
    }
    if (!Write) {
        if (h->Resp.Dw < 3 + Len) {
            return -1;
        }
        for (i = 0; i < Len; i++) {
            Data[i] = h->Resp.D[3 + i];
        }
    }
    return 0;
}

static int Rd1(TB_HOST *h, TB_ROUTER *r, UCHAR Space, UCHAR Port, ULONG Off, ULONG *V)
{
    return Cfg(h, r->Route, Space, Port, Off, V, 1, FALSE, NULL, 1000);
}

static int Wr1(TB_HOST *h, TB_ROUTER *r, UCHAR Space, UCHAR Port, ULONG Off, ULONG V)
{
    return Cfg(h, r->Route, Space, Port, Off, &V, 1, TRUE, NULL, 1000);
}

/* waits for bits of a router register (ms) */
static BOOLEAN WaitBits(TB_HOST *h, TB_ROUTER *r, UCHAR Space, UCHAR Port, ULONG Off, ULONG Mask, ULONG Val,
                        ULONG Ms)
{
    ULONG start = OsTimeMs(), v;

    for (;;) {
        if (Rd1(h, r, Space, Port, Off, &v) == 0 && (v & Mask) == Val) {
            return TRUE;
        }
        if (OsTimeMs() - start > Ms) {
            return FALSE;
        }
        OsSleepMs(5);
    }
}

/* ------------------------------------------------------------------ */
/* firmware connection manager (ICM)                                    */
/* ------------------------------------------------------------------ */

static void IcmSend(TB_HOST *h, int Dev, const ULONG *Req, int Dw, ULONG TimeoutMs, int Tries)
{
    UsbMemCpy(h->IcmReq, Req, (ULONG)Dw * 4);
    h->IcmReqDw = Dw;
    h->IcmCode = (UCHAR)(Req[0] & 0xFF);
    h->IcmDev = Dev;
    h->IcmTimeout = TimeoutMs;
    h->IcmTries = Tries;
    h->IcmBusy = TRUE;
    h->IcmDone = FALSE;
    h->IcmSent = OsTimeMs();
    Tx(h, PKG_ICM_CMD, Req, Dw);
}

static void KeyName(TB_DEV *d, char *Name)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    Name[0] = 'T';
    Name[1] = 'b';
    Name[2] = 't';
    for (i = 0; i < 16; i++) {
        Name[3 + 2 * i] = hex[d->Uuid[i] >> 4];
        Name[4 + 2 * i] = hex[d->Uuid[i] & 15];
    }
    Name[35] = 0;
}

static void UuidStr(const UCHAR *U, char *S)
{
    UsbFormat(S, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", U[3], U[2], U[1], U[0],
              U[5], U[4], U[7], U[6], U[8], U[9], U[10], U[11], U[12], U[13], U[14], U[15]);
}

static BOOLEAN FrMsgs(TB_HOST *h)
{
    return h->Gen == GEN_FR || h->Gen == GEN_AR;
}

/* the part after the header that names the device in requests */
static int DevId(TB_HOST *h, TB_DEV *d, ULONG *M)
{
    UsbMemCpy(&M[1], d->Uuid, 16);
    if (FrMsgs(h)) {
        M[5] = d->ConnKey | ((ULONG)d->ConnId << 8);
        return 6;
    }
    M[5] = d->RouteHi;
    M[6] = d->RouteLo;
    M[7] = d->ConnId;
    return 8;
}

static void IcmNext(TB_HOST *h, int i)
{
    TB_DEV *d = &h->D[i];
    ULONG m[32];
    int n;

    UsbMemSet(m, 0, sizeof(m));
    n = DevId(h, d, m);
    switch (d->State) {
    case DS_APPROVE:
        m[0] = ICM_APPROVE_DEVICE;
        IcmSend(h, i, m, n, 10000, 1);
        break;
    case DS_ADDKEY:
        m[0] = ICM_ADD_DEVICE_KEY;
        UsbMemCpy(&m[n], d->Key, 32);
        IcmSend(h, i, m, n + 8, 5000, 3);
        break;
    case DS_CHALLENGE:
        m[0] = ICM_CHALLENGE_DEVICE;
        Random(d->Challenge, 32);
        UsbMemCpy(&m[n], d->Challenge, 32);
        IcmSend(h, i, m, n + 8, 5000, 3);
        break;
    default:
        return;
    }
    d->State = DS_WAIT;
}

static void IcmConnected(TB_HOST *h, TB_PKT *p)
{
    const UCHAR *b = (const UCHAR *)p->D;
    UCHAR flags = b[1];
    TB_DEV *d = NULL;
    USHORT link;
    const UCHAR *name;
    int i, nameLen;
    char u[40], kn[36];
    ULONG policy = OsGetConfig("TbtApprove", 1);

    if (b[2] != 0) {
        return;                                 /* additional packets (QoS) */
    }
    if (FrMsgs(h)) {
        link = (USHORT)(b[22] | (b[23] << 8));
        name = b + 24;
    } else {
        link = (USHORT)(b[30] | (b[31] << 8));
        name = b + 32;
    }
    nameLen = p->Dw * 4 - (int)(name - b);
    UuidStr(b + 4, u);
    if (link & LINK_REJECTED) {
        UsbLog(LOG_ERR, "%s: device %s rejected by the firmware (too many devices in the chain)\n", h->Name, u);
        return;
    }
    for (i = 0; i < TB_DEVS; i++) {
        if (h->D[i].Used && UsbMemCmp(h->D[i].Uuid, b + 4, 16) == 0) {
            d = &h->D[i];
        }
    }
    if (d == NULL) {
        for (i = 0; i < TB_DEVS && d == NULL; i++) {
            if (!h->D[i].Used) {
                d = &h->D[i];
            }
        }
    }
    if (d == NULL) {
        UsbLog(LOG_ERR, "%s: more than %u devices\n", h->Name, TB_DEVS);
        return;
    }
    UsbMemSet(d, 0, sizeof(*d));
    d->Used = TRUE;
    UsbMemCpy(d->Uuid, b + 4, 16);
    if (FrMsgs(h)) {
        d->ConnKey = b[20];
        d->ConnId = b[21];
    } else {
        d->RouteHi = p->D[5];
        d->RouteLo = p->D[6];
        d->ConnId = b[28];
    }
    d->LinkInfo = link;
    d->Sl = (UCHAR)((flags >> 3) & 3);
    d->Gbps = (UCHAR)(((flags & 0x80) ? 20 : 10) * ((flags & 0x20) ? 2 : 1));
    /* Intel vendor specific entry of the name: vendor and model */
    for (i = 0; i + 2 <= nameLen && name[i] != 0 && i + name[i] <= nameLen;) {
        if (name[i + 1] == 0x10 && name[i] >= 6) {
            d->Vendor = (USHORT)(name[i + 2] | (name[i + 3] << 8));
            d->Model = (USHORT)(name[i + 4] | (name[i + 5] << 8));
            break;
        }
        i += name[i];
    }
    UsbLog(LOG_INFO, "%s: device %s connected (vendor %04x model %04x, link %u depth %u, %u Gb/s%s%s, security %s)\n",
           h->Name, u, d->Vendor, d->Model, link & 7, (link >> 4) & 15, d->Gbps,
           (link & LINK_APPROVED) ? ", approved" : "", (link & LINK_BOOT) ? ", by the BIOS" : "", SlName[d->Sl]);
    KeyName(d, kn);
    d->HaveKey = OsStoreGet(kn, d->Key, 32);
    if (link & LINK_APPROVED) {
        d->State = DS_DONE;
        UsbFormat(d->Note, sizeof(d->Note), "connected");
    } else if (h->Gen == GEN_ICL) {
        d->State = DS_DONE;
        UsbFormat(d->Note, sizeof(d->Note), "waits for the firmware");
        UsbLog(LOG_INFO, "%s: this firmware connects devices by its own policy (BIOS setup)\n", h->Name);
    } else if (d->Sl == 3 || d->Sl == 4 || d->Sl == 5) {
        d->State = DS_REFUSED;
        UsbFormat(d->Note, sizeof(d->Note), "no PCIe (security %s)", SlName[d->Sl]);
        UsbLog(LOG_INFO, "%s: security level %s: no PCIe tunnel for %s\n", h->Name, SlName[d->Sl], u);
    } else if (policy == 0 || (policy == 2 && !d->HaveKey)) {
        d->State = DS_REFUSED;
        UsbFormat(d->Note, sizeof(d->Note), "not approved (TbtApprove)");
        UsbLog(LOG_INFO, "%s: %s not approved (TbtApprove = %u)\n", h->Name, u, policy);
    } else if (d->Sl == 2) {
        if (d->HaveKey) {
            d->State = DS_CHALLENGE;
        } else {
            Random(d->Key, 32);
            d->State = DS_ADDKEY;
        }
    } else {
        d->State = DS_APPROVE;
    }
}

static void IcmDisconnected(TB_HOST *h, TB_PKT *p)
{
    const UCHAR *b = (const UCHAR *)p->D;
    int i;

    for (i = 0; i < TB_DEVS; i++) {
        TB_DEV *d = &h->D[i];
        BOOLEAN match;
        char u[40];
        if (!d->Used) {
            continue;
        }
        if (FrMsgs(h)) {
            USHORT link = (USHORT)(b[6] | (b[7] << 8));
            /* the device itself or one behind it (same link, deeper) */
            match = (d->LinkInfo & 7) == (link & 7) && ((d->LinkInfo >> 4) & 15) >= ((link >> 4) & 15);
        } else {
            ULONG lo = p->Dw >= 3 ? p->D[2] : 0, hi = p->Dw >= 2 ? p->D[1] : 0;
            ULONG mask = 0;
            int k;
            for (k = 0; k < 4; k++) {
                if ((lo >> (8 * k)) & 0xFF) {
                    mask |= 0xFFUL << (8 * k);
                }
            }
            match = d->RouteHi == hi && (d->RouteLo & mask) == lo;
        }
        if (match) {
            UuidStr(d->Uuid, u);
            UsbLog(LOG_INFO, "%s: device %s disconnected\n", h->Name, u);
            if (h->IcmBusy && h->IcmDev == i) {
                h->IcmBusy = FALSE;
            }
            d->Used = FALSE;
        }
    }
    UsbNotifyChange();
}

static void IcmReply(TB_HOST *h)
{
    TB_DEV *d = &h->D[h->IcmDev];
    const UCHAR *b = (const UCHAR *)h->IcmResp.D;
    UCHAR flags = b[1];
    char u[40], kn[36];
    UCHAR mac[32];

    h->IcmBusy = FALSE;
    UuidStr(d->Uuid, u);
    KeyName(d, kn);
    switch (h->IcmCode) {
    case ICM_APPROVE_DEVICE:
        if (flags & ICM_FL_ERROR) {
            UsbLog(LOG_ERR, "%s: %s: the firmware could not set up the PCIe tunnel\n", h->Name, u);
            d->State = DS_REFUSED;
            UsbFormat(d->Note, sizeof(d->Note), "PCIe tunnel failed");
        } else {
            UsbLog(LOG_INFO, "%s: %s approved, PCIe tunnel up\n", h->Name, u);
            d->State = DS_DONE;
            UsbFormat(d->Note, sizeof(d->Note), d->Sl != 2 ? "connected" : d->NewKey ? "connected, paired" :
                      "connected, key checked");
        }
        break;
    case ICM_ADD_DEVICE_KEY:
        if (flags & ICM_FL_ERROR) {
            UsbLog(LOG_ERR, "%s: %s did not take a key\n", h->Name, u);
            d->State = DS_REFUSED;
            UsbFormat(d->Note, sizeof(d->Note), "key not accepted");
        } else {
            if (!OsStoreSet(kn, d->Key, 32)) {
                UsbLog(LOG_ERR, "%s: the key of %s could not be saved, it pairs again next time\n", h->Name, u);
            }
            d->HaveKey = TRUE;
            d->NewKey = TRUE;
            UsbLog(LOG_INFO, "%s: %s paired (new key)\n", h->Name, u);
            d->State = DS_APPROVE;
        }
        break;
    case ICM_CHALLENGE_DEVICE:
        if (flags & ICM_FL_NO_KEY) {
            UsbLog(LOG_INFO, "%s: %s has lost its key, pairing again\n", h->Name, u);
            Random(d->Key, 32);
            d->State = DS_ADDKEY;
        } else if (flags & ICM_FL_ERROR) {
            UsbLog(LOG_ERR, "%s: %s refused the challenge; not connected (delete %s under Parameters to pair "
                   "again)\n", h->Name, u, kn);
            d->State = DS_REFUSED;
            UsbFormat(d->Note, sizeof(d->Note), "challenge refused");
        } else {
            int off = FrMsgs(h) ? 24 : 32;
            HmacSha256(d->Key, 32, d->Challenge, 32, mac);
            if (h->IcmResp.Dw * 4 < off + 64 || UsbMemCmp(b + off, d->Challenge, 32) != 0 ||
                UsbMemCmp(b + off + 32, mac, 32) != 0) {
                UsbLog(LOG_ERR, "%s: %s answered the challenge with a wrong key; not connected\n", h->Name, u);
                d->State = DS_REFUSED;
                UsbFormat(d->Note, sizeof(d->Note), "wrong key");
            } else {
                UsbLog(LOG_INFO, "%s: %s: key checked\n", h->Name, u);
                d->State = DS_APPROVE;
            }
        }
        break;
    default:
        break;
    }
    UsbNotifyChange();
}

/* ------------------------------------------------------------------ */
/* software connection manager (USB4)                                   */
/* ------------------------------------------------------------------ */

static void ReadPorts(TB_HOST *h, TB_ROUTER *r)
{
    ULONG hdr[8], v;
    int p, k, i;

    for (p = 0; p <= r->MaxPort && p < TB_PORTS; p++) {
        TB_PORT *pt = &r->P[p];
        ULONG off;
        UsbMemSet(pt, 0, sizeof(*pt));
        if (Cfg(h, r->Route, SP_PORT, (UCHAR)p, 0, hdr, 8, FALSE, NULL, 1000) != 0) {
            continue;
        }
        pt->Type = hdr[2] & 0xFFFFFF;
        pt->MaxInHop = hdr[5] & 0x7FF;
        off = hdr[1] & 0xFF;
        for (k = 0; k < 16 && off != 0; k++) {
            UCHAR id;
            if (Cfg(h, r->Route, SP_PORT, (UCHAR)p, off, &v, 1, FALSE, NULL, 1000) != 0) {
                break;
            }
            id = (UCHAR)(v >> 8);
            if (id == CAP_PHY) {
                pt->CapPhy = (UCHAR)off;
            } else if (id == CAP_USB4) {
                pt->CapUsb4 = (UCHAR)off;
            } else if (id == CAP_ADAP) {
                pt->CapAdap = (UCHAR)off;
            }
            off = v & 0xFF;
        }
    }
    /* lane adapters pair up as the two lanes of one port: 1 + 2, 3 + 4 ... */
    for (i = 1; i < r->MaxPort && i + 1 < TB_PORTS;) {
        if (r->P[i].Type == PT_LANE && r->P[i + 1].Type == PT_LANE) {
            r->P[i + 1].Lane1 = TRUE;
            i += 2;
        } else {
            i++;
        }
    }
}

static int PortState(TB_HOST *h, TB_ROUTER *r, UCHAR p)
{
    ULONG v;

    if (r->P[p].CapPhy == 0 || Rd1(h, r, SP_PORT, p, r->P[p].CapPhy + 1, &v) != 0) {
        return -1;
    }
    return (int)((v >> 26) & 0xF);
}

/* the n-th adapter of a type, n = index of the USB4 port the lane adapter belongs to */
static UCHAR MapDown(TB_ROUTER *r, UCHAR Lane, ULONG Type)
{
    int idx = 0, n = 0, p;

    for (p = 1; p <= r->MaxPort && p < TB_PORTS; p++) {
        if (r->P[p].Type == PT_LANE && !r->P[p].Lane1 && p != r->Up) {
            if (p == Lane) {
                break;
            }
            idx++;
        }
    }
    for (p = 1; p <= r->MaxPort && p < TB_PORTS; p++) {
        if (r->P[p].Type == Type) {
            if (n == idx) {
                return (UCHAR)p;
            }
            n++;
        }
    }
    return 0;
}

static UCHAR FindType(TB_ROUTER *r, ULONG Type)
{
    int p;

    for (p = 1; p <= r->MaxPort && p < TB_PORTS; p++) {
        if (r->P[p].Type == Type) {
            return (UCHAR)p;
        }
    }
    return 0;
}

/* Thunderbolt 3 router: vendor specific capability of the switch space */
static ULONG FindVse(TB_HOST *h, TB_ROUTER *r, UCHAR Vsec)
{
    ULONG cs1, d[2], off;
    int k;

    if (Rd1(h, r, SP_SWITCH, 0, 1, &cs1) != 0) {
        return 0;
    }
    off = cs1 & 0xFF;
    for (k = 0; k < 16 && off != 0 && off < 0xFFFF; k++) {
        if (Cfg(h, r->Route, SP_SWITCH, 0, off, d, 2, FALSE, NULL, 1000) != 0) {
            return 0;
        }
        if (((d[0] >> 8) & 0xFF) == CAP_VSE) {
            if (((d[0] >> 16) & 0xFF) == Vsec) {
                return off;
            }
            off = (d[0] >> 24) == 0 ? (d[1] & 0xFFFF) : (d[0] & 0xFF);
        } else {
            off = d[0] & 0xFF;
        }
    }
    return 0;
}

/* Thunderbolt 3 device router: lane configured (link controller) */
/* Thunderbolt generation of a router (1 .. 3; 4 = USB4): link controller and bonding from 2 on */
static UCHAR RouterGen(TB_ROUTER *r)
{
    if (r->Version != 0) {
        return 4;
    }
    if ((r->Id & 0xFFFF) == 0x8086) {
        switch (r->Id >> 16) {
        case 0x1513: case 0x151A: case 0x151B: case 0x1547: case 0x1548: case 0x1549: case 0x1567: case 0x1569:
            return 1;
        case 0x156B: case 0x156D: case 0x157E:
            return 2;
        }
    }
    return 3;
}

static const char *RouterKind(TB_ROUTER *r)
{
    static const char *k[] = { "", "Thunderbolt 1 router", "Thunderbolt 2 router", "Thunderbolt 3 router" };

    if (r->Version >= 2) {
        return "USB4 v2 router";
    }
    return r->Version ? "USB4 router" : k[RouterGen(r)];
}

static void Tb3LinkConfigured(TB_HOST *h, TB_ROUTER *r, UCHAR Port, BOOLEAN Upstream)
{
    ULONG lc, desc, ctl, cap;

    if (RouterGen(r) < 2 || (lc = FindVse(h, r, VSE_LC)) == 0 || Rd1(h, r, SP_SWITCH, 0, lc + LC_DESC, &desc) != 0) {
        return;                                 /* Thunderbolt 1 has no link controller to tell */
    }
    /* descriptor: link controllers (3:0), start (15:8), size per link controller (27:16) */
    cap = lc + ((desc >> 8) & 0xFF) + ((ULONG)(Port - 1) / 2) * ((desc >> 16) & 0xFFF);
    if (Rd1(h, r, SP_SWITCH, 0, cap + LC_SX_CTRL, &ctl) != 0) {
        return;
    }
    ctl |= (Port & 1) ? LC_SX_L1C : LC_SX_L2C;
    if (Upstream) {
        ctl |= LC_SX_UPSTREAM;
    }
    Wr1(h, r, SP_SWITCH, 0, cap + LC_SX_CTRL, ctl);
}

static UCHAR AllocLinkHop(TB_ROUTER *a, UCHAR OutA, TB_ROUTER *b, UCHAR InB)
{
    UCHAR hop;

    for (hop = HOPID_MIN; hop < 64; hop++) {
        if (!(a->P[OutA].HopOut & (1ULL << hop)) && !(b->P[InB].HopIn & (1ULL << hop)) &&
            (b->P[InB].MaxInHop == 0 || hop <= b->P[InB].MaxInHop)) {
            a->P[OutA].HopOut |= 1ULL << hop;
            b->P[InB].HopIn |= 1ULL << hop;
            return hop;
        }
    }
    return 0;
}

static int WriteHop(TB_HOST *h, TB_ROUTER *r, UCHAR In, UCHAR InHop, UCHAR Out, UCHAR NextHop, BOOLEAN Last,
                    UCHAR Prio, UCHAR Weight)
{
    ULONG hop[2] = { 0, 0 };
    BOOLEAN lane = r->P[In].Type == PT_LANE;

    /* whatever was there goes first */
    Cfg(h, r->Route, SP_HOPS, In, 2UL * InHop, hop, 2, TRUE, NULL, 1000);
    hop[0] = (NextHop & 0x7FFUL) | ((ULONG)(Out & 0x3F) << 11) | 0x80000000UL;
    hop[1] = (Weight & 0xFUL) | ((ULONG)(Prio & 7) << 8) | (0x7FFUL << 12);
    if (lane || r->Version == 0) {
        hop[0] |= (ULONG)(lane ? (r->P[In].Bonded ? 32 : 16) : 7) << 17;   /* initial credits */
        hop[1] |= 1UL << 24;                            /* ingress flow control */
    }
    if (!Last) {
        hop[1] |= 1UL << 25;                            /* egress flow control */
    }
    return Cfg(h, r->Route, SP_HOPS, In, 2UL * InHop, hop, 2, TRUE, NULL, 1000);
}

static void ClearHop(TB_HOST *h, TB_ROUTER *r, UCHAR In, UCHAR InHop)
{
    ULONG hop[2];
    ULONG start = OsTimeMs();

    if (Cfg(h, r->Route, SP_HOPS, In, 2UL * InHop, hop, 2, FALSE, NULL, 1000) != 0 || !(hop[0] & 0x80000000UL)) {
        return;
    }
    hop[0] &= ~0x80000000UL;
    Cfg(h, r->Route, SP_HOPS, In, 2UL * InHop, hop, 2, TRUE, NULL, 1000);
    while (OsTimeMs() - start < 100) {
        if (Cfg(h, r->Route, SP_HOPS, In, 2UL * InHop, hop, 2, FALSE, NULL, 1000) != 0 || !(hop[1] & (1UL << 28))) {
            break;
        }
        OsSleepMs(2);
    }
    hop[0] = hop[1] = 0;
    Cfg(h, r->Route, SP_HOPS, In, 2UL * InHop, hop, 2, TRUE, NULL, 1000);
}

/* two hop paths between neighbours: parent adapter <-> lane <-> lane <-> child adapter */
static BOOLEAN Paths(TB_HOST *h, TB_ROUTER *pr, UCHAR PDown, TB_ROUTER *r, UCHAR RUp, UCHAR HopId, UCHAR Prio,
                     UCHAR Weight, UCHAR *LD, UCHAR *LU)
{
    UCHAR ld = AllocLinkHop(pr, r->ParentPort, r, r->Up);
    UCHAR lu = AllocLinkHop(r, r->Up, pr, r->ParentPort);

    *LD = ld;
    *LU = lu;
    if (ld == 0 || lu == 0) {
        return FALSE;
    }
    return WriteHop(h, pr, PDown, HopId, r->ParentPort, ld, FALSE, Prio, Weight) == 0 &&
           WriteHop(h, r, r->Up, ld, RUp, HopId, TRUE, Prio, Weight) == 0 &&
           WriteHop(h, r, RUp, HopId, r->Up, lu, FALSE, Prio, Weight) == 0 &&
           WriteHop(h, pr, r->ParentPort, lu, PDown, HopId, TRUE, Prio, Weight) == 0;
}

static ULONG Usb3Rate(TB_HOST *h, TB_ROUTER *r, UCHAR p)
{
    ULONG v;

    if (Rd1(h, r, SP_PORT, p, r->P[p].CapAdap + 4UL, &v) != 0) {
        return 10000;
    }
    return ((v >> 12) & 0x7F) == 1 ? 20000 : 10000;
}

static ULONG Usb3Units(ULONG Mbps, ULONG Scale)
{
    ULONG uframes = Mbps * 125;                         /* Mb/s -> bytes per 125 us, in 512 << Scale units */
    ULONG unit = 512UL << Scale;
    return (uframes + unit - 1) / unit;
}

/* USB 3 bandwidth of the host router's adapter (connection manager request handshake) */
static BOOLEAN Usb3Bandwidth(TB_HOST *h, TB_ROUTER *r, UCHAR p, ULONG Mbps)
{
    ULONG cap = r->P[p].CapAdap, v, scale = 0;
    BOOLEAN ok = FALSE;

    if (Rd1(h, r, SP_PORT, p, cap + 2, &v) != 0 || Wr1(h, r, SP_PORT, p, cap + 2, v | USB3_CMR) != 0 ||
        !WaitBits(h, r, SP_PORT, p, cap + 1, USB3_HCA, USB3_HCA, 1500)) {
        UsbLog(LOG_ERR, "%s: USB 3 bandwidth request not acknowledged\n", h->Name);
    } else {
        while (scale < 63 && Usb3Units(Mbps, scale) >= 4096) {
            scale++;
        }
        Wr1(h, r, SP_PORT, p, cap + 3, scale);
        if (Rd1(h, r, SP_PORT, p, cap + 2, &v) == 0) {
            v &= ~0x00FFFFFFUL;
            v |= Usb3Units(Mbps, scale) | (Usb3Units(Mbps, scale) << 12);
            ok = Wr1(h, r, SP_PORT, p, cap + 2, v) == 0;
        }
    }
    if (Rd1(h, r, SP_PORT, p, cap + 2, &v) == 0) {
        Wr1(h, r, SP_PORT, p, cap + 2, v & ~USB3_CMR);
        WaitBits(h, r, SP_PORT, p, cap + 1, USB3_HCA, 0, 1500);
    }
    return ok;
}

static void TunnelUsb3(TB_HOST *h, TB_ROUTER *pr, TB_ROUTER *r)
{
    UCHAR up = FindType(r, PT_USB3_UP), down;
    ULONG v, rate;

    if (up == 0 || r->Version == 0 || !r->LinkUsb4 || pr->Version == 0 || OsGetConfig("TbtUsb3Tunnel", 1) == 0) {
        return;
    }
    down = MapDown(pr, r->ParentPort, PT_USB3_DOWN);
    if (down == 0 || pr->P[down].CapAdap == 0 || r->P[up].CapAdap == 0) {
        return;
    }
    if (Rd1(h, pr, SP_PORT, down, pr->P[down].CapAdap, &v) == 0 && (v & USB3_PE)) {
        UsbLog(LOG_INFO, "%s: USB 3 adapter %u already in use\n", h->Name, down);
        return;
    }
    if (pr->Depth == 0) {
        rate = Usb3Rate(h, pr, down);
        if (Usb3Rate(h, r, up) < rate) {
            rate = Usb3Rate(h, r, up);
        }
        if (!Usb3Bandwidth(h, pr, down, rate * 90 / 100)) {
            return;
        }
    }
    if (!Paths(h, pr, down, r, up, USB3_HOPID, 3, 2, &r->Usb3LD, &r->Usb3LU)) {
        UsbLog(LOG_ERR, "%s: USB 3 tunnel paths could not be set up\n", h->Name);
        return;
    }
    Wr1(h, pr, SP_PORT, down, pr->P[down].CapAdap, USB3_PE | USB3_V);
    Wr1(h, r, SP_PORT, up, r->P[up].CapAdap, USB3_PE | USB3_V);
    r->Usb3Down = down;
    r->Usb3Up = up;
    UsbLog(LOG_INFO, "%s: USB 3 tunnel %x:%u <-> %x:%u\n", h->Name, pr->Route, down, r->Route, up);
}

static void WaitLtssmDetect(TB_HOST *h, TB_ROUTER *r, UCHAR p)
{
    ULONG start = OsTimeMs(), v;

    if (r->Version == 0) {
        return;
    }
    while (Rd1(h, r, SP_PORT, p, r->P[p].CapAdap, &v) == 0 && ((v >> 25) & 0xF) != 0) {
        if (OsTimeMs() - start > 200) {
            UsbLog(LOG_INFO, "%s: PCIe adapter %x:%u not in detect state (%u)\n", h->Name, r->Route, p,
                   (v >> 25) & 0xF);
            return;
        }
        OsSleepMs(5);
    }
}

static void TunnelPcie(TB_HOST *h, TB_ROUTER *pr, TB_ROUTER *r)
{
    UCHAR up = FindType(r, PT_PCIE_UP), down;
    ULONG v;

    if (up == 0) {
        return;
    }
    down = MapDown(pr, r->ParentPort, PT_PCIE_DOWN);
    if (down == 0 || pr->P[down].CapAdap == 0 || r->P[up].CapAdap == 0) {
        UsbLog(LOG_ERR, "%s: no PCIe downstream adapter for router %x\n", h->Name, r->Route);
        return;
    }
    if (Rd1(h, pr, SP_PORT, down, pr->P[down].CapAdap, &v) == 0 && (v & PCIE_PE)) {
        UsbLog(LOG_INFO, "%s: PCIe adapter %u already in use\n", h->Name, down);
        return;
    }
    WaitLtssmDetect(h, pr, down);
    WaitLtssmDetect(h, r, up);
    if (!Paths(h, pr, down, r, up, PCIE_HOPID, 3, 1, &r->PcieLD, &r->PcieLU)) {
        UsbLog(LOG_ERR, "%s: PCIe tunnel paths could not be set up\n", h->Name);
        return;
    }
    /* USB4 v2 routers on a Gen 4 link: extended encapsulation (larger PCIe packets), both ends, before PE */
    r->PcieEe = FALSE;
    if (pr->Version >= 2 && r->Version >= 2 && r->LinkGbps >= 40 && OsGetConfig("TbtPcieExtEncap", 1)) {
        if (Rd1(h, pr, SP_PORT, down, pr->P[down].CapAdap + 1, &v) == 0) {
            Wr1(h, pr, SP_PORT, down, pr->P[down].CapAdap + 1, v | PCIE_CS_1_EE);
        }
        if (Rd1(h, r, SP_PORT, up, r->P[up].CapAdap + 1, &v) == 0) {
            Wr1(h, r, SP_PORT, up, r->P[up].CapAdap + 1, v | PCIE_CS_1_EE);
        }
        r->PcieEe = TRUE;
    }
    Wr1(h, r, SP_PORT, up, r->P[up].CapAdap, PCIE_PE);
    Wr1(h, pr, SP_PORT, down, pr->P[down].CapAdap, PCIE_PE);
    r->PcieDown = down;
    r->PcieUp = up;
    UsbLog(LOG_INFO, "%s: PCIe tunnel %x:%u <-> %x:%u%s\n", h->Name, pr->Route, down, r->Route, up,
           r->PcieEe ? " (extended encapsulation)" : "");
}

/* lane adapter: current speed per lane (Gb/s) and width (1, 2) */
static void LinkState(TB_HOST *h, TB_ROUTER *r, UCHAR p, UCHAR *Gbps, UCHAR *Lanes)
{
    ULONG v = 0;

    *Gbps = 0;
    *Lanes = 0;
    if (r->P[p].CapPhy == 0 || Rd1(h, r, SP_PORT, p, r->P[p].CapPhy + 1, &v) != 0) {
        return;
    }
    switch ((v >> 16) & 0xF) {
    case 0x2: *Gbps = 40; break;                        /* Gen 4 */
    case 0x4: *Gbps = 20; break;                        /* Gen 3 */
    default: *Gbps = 10; break;                         /* Gen 2 */
    }
    *Lanes = (UCHAR)((v >> 20) & 0x3F) >= 2 ? 2 : 1;
}

static UCHAR OtherLane(TB_ROUTER *r, UCHAR p)
{
    if (r->P[p].Lane1) {
        return (UCHAR)(p - 1);
    }
    if (p + 1 < TB_PORTS && r->P[p + 1].Lane1) {
        return (UCHAR)(p + 1);
    }
    return 0;
}

static BOOLEAN DualSupported(TB_HOST *h, TB_ROUTER *r, UCHAR p)
{
    ULONG v;

    return r->P[p].CapPhy != 0 && Rd1(h, r, SP_PORT, p, r->P[p].CapPhy, &v) == 0 && (((v >> 20) & 0x3F) & 2);
}

/* target width of a lane adapter: 1 single, 3 dual */
static void SetWidth(TB_HOST *h, TB_ROUTER *r, UCHAR p, ULONG W)
{
    ULONG v;

    if (Rd1(h, r, SP_PORT, p, r->P[p].CapPhy + 1, &v) == 0) {
        Wr1(h, r, SP_PORT, p, r->P[p].CapPhy + 1, (v & ~0x30UL) | (W << 4));
    }
}

/* 40 Gb/s: the two lanes of the link to the parent bonded into one (both ends dual, LB on the downstream end) */
static void Bond(TB_HOST *h, TB_ROUTER *pr, TB_ROUTER *r)
{
    UCHAR up = r->Up, down = r->ParentPort, up1 = OtherLane(r, r->Up), down1 = OtherLane(pr, r->ParentPort);
    UCHAR g, w;
    ULONG v, start;
    BOOLEAN possible = FALSE;

    LinkState(h, r, up, &r->LinkGbps, &r->LinkLanes);
    if (r->LinkLanes >= 2) {
        /* USB4 v2 Gen 4 (Thunderbolt 5) links come up bonded (also one bonded before, by the firmware):
           the paths get the credits of a bonded link */
        r->HwBonded = r->LinkGbps >= 40;        /* a Gen 4 link cannot be split again */
        r->P[up].Bonded = TRUE;
        pr->P[down].Bonded = TRUE;
        if (up1) r->P[up1].Bonded = TRUE;
        if (down1) pr->P[down1].Bonded = TRUE;
        goto out;
    }
    if (up1 == 0 || down1 == 0 || OsGetConfig("TbtLaneBonding", 1) == 0 || RouterGen(r) < 2 || RouterGen(pr) < 2) {
        goto out;
    }
    if (r->Version != 0) {
        possible = r->P[up].CapUsb4 && Rd1(h, r, SP_PORT, up, r->P[up].CapUsb4 + PORT_CS_18, &v) == 0 &&
                   (v & PORT_CS_18_BE);
    } else {
        ULONG lc = FindVse(h, r, VSE_LC), desc;
        if (lc && Rd1(h, r, SP_SWITCH, 0, lc + LC_DESC, &desc) == 0) {
            ULONG cap = lc + ((desc >> 8) & 0xFF) + ((ULONG)(up - 1) / 2) * ((desc >> 16) & 0xFFF);
            possible = Rd1(h, r, SP_SWITCH, 0, cap + LC_PORT_ATTR, &v) == 0 && (v & LC_PORT_ATTR_BE);
        }
    }
    if (!possible || !DualSupported(h, r, up) || !DualSupported(h, pr, down) || PortState(h, pr, down1) != PORT_STATE_UP) {
        goto out;
    }
    SetWidth(h, r, up, 3);
    SetWidth(h, r, up1, 3);
    SetWidth(h, pr, down, 3);
    SetWidth(h, pr, down1, 3);
    if (Rd1(h, pr, SP_PORT, down, pr->P[down].CapPhy + 1, &v) == 0) {
        Wr1(h, pr, SP_PORT, down, pr->P[down].CapPhy + 1, v | LANE_LB);
    }
    start = OsTimeMs();
    do {
        OsSleepMs(2);
        LinkState(h, pr, down, &g, &w);
    } while (w < 2 && OsTimeMs() - start < 100);
    if (w < 2) {
        UsbLog(LOG_INFO, "%s: lanes to router %x not bonded, staying on one lane\n", h->Name, r->Route);
        if (Rd1(h, pr, SP_PORT, down, pr->P[down].CapPhy + 1, &v) == 0) {
            Wr1(h, pr, SP_PORT, down, pr->P[down].CapPhy + 1, v & ~LANE_LB);
        }
        SetWidth(h, pr, down1, 1);
        SetWidth(h, pr, down, 1);
        SetWidth(h, r, up1, 1);
        SetWidth(h, r, up, 1);
    } else {
        r->P[up].Bonded = r->P[up1].Bonded = TRUE;
        pr->P[down].Bonded = pr->P[down1].Bonded = TRUE;
    }
    LinkState(h, r, up, &r->LinkGbps, &r->LinkLanes);
out:
    if (r->LinkGbps) {
        UsbLog(LOG_INFO, "%s: link to router %x: %u Gb/s x %u = %u Gb/s%s\n", h->Name, r->Route, r->LinkGbps,
               r->LinkLanes, r->LinkGbps * r->LinkLanes, r->LinkGbps >= 40 ? " (Gen 4, USB4 v2 / Thunderbolt 5)" : "");
    }
}

/* Thunderbolt 1 / 2 / 3 router: hot plug events on (the internal USB ones off, except Thunderbolt 1, which
   has none, and Alpine Ridge, whose xHCI needs them) */
static void PlugEventsOn(TB_HOST *h, TB_ROUTER *r)
{
    ULONG pe = FindVse(h, r, VSE_PLUG_EVENTS), v;
    USHORT id = (USHORT)(r->Id >> 16);

    if (pe == 0 || Rd1(h, r, SP_SWITCH, 0, pe + 1, &v) != 0) {
        return;
    }
    v &= 0xFFFFFF83UL;
    if (RouterGen(r) != 1 && id != 0x1578 && id != 0x1576 && id != 0x15C0 && id != 0x15D3 && id != 0x15DA) {
        v |= 0x04;
    }
    Wr1(h, r, SP_SWITCH, 0, pe + 1, v);
}

/* ROUTER_CS_4: notification timeout 255 ms; USB4 routers: the USB4 version the connection manager knows (v2
   routers hide their v2 features from a v1 one) */
static ULONG Cs4(TB_ROUTER *r, ULONG Cs4)
{
    return (Cs4 & 0xFFFF0000UL) | 0xFF |
           (r->Version == 0 ? (Cs4 & 0xFF00) : ((ULONG)(r->Version >= 2 ? CS_4_CMUV_V2 : CS_4_CMUV_V1) << 8));
}

static void Enumerate(TB_HOST *h, int Pi, UCHAR Port);

static void ScanRouter(TB_HOST *h, int Ri)
{
    TB_ROUTER *r = &h->R[Ri];
    int p, pair = -1;

    if (Ri != 0) {
        /* the upstream port: its own lane and the other lane of it */
        pair = r->P[r->Up].Lane1 ? r->Up - 1 : (r->Up + 1 < TB_PORTS && r->P[r->Up + 1].Lane1 ? r->Up + 1 : -1);
    }
    for (p = 1; p <= r->MaxPort && p < TB_PORTS; p++) {
        TB_PORT *pt = &r->P[p];
        if (pt->Type != PT_LANE || pt->Lane1 || pt->Child || (Ri != 0 && (p == r->Up || p == pair))) {
            continue;
        }
        if (PortState(h, r, (UCHAR)p) == PORT_STATE_UP) {
            Enumerate(h, Ri, (UCHAR)p);
        }
    }
}

static void Enumerate(TB_HOST *h, int Pi, UCHAR Port)
{
    TB_ROUTER *pr = &h->R[Pi], *r = NULL;
    ULONG route, cs[5], v;
    UCHAR up = 0;
    int i, k, ri = -1;
    ULONG start;

    if (pr->Depth >= 4) {
        UsbLog(LOG_ERR, "%s: chain deeper than 4 routers\n", h->Name);
        return;
    }
    for (i = 1; i < TB_ROUTERS && ri < 0; i++) {
        if (!h->R[i].Used) {
            ri = i;
        }
    }
    if (ri < 0) {
        UsbLog(LOG_ERR, "%s: more than %u routers\n", h->Name, TB_ROUTERS - 1);
        return;
    }
    route = pr->Route | ((ULONG)Port << (8 * pr->Depth));
    /* the downstream router can be reached once the lane is unlocked */
    if (pr->Version != 0 && Rd1(h, pr, SP_PORT, Port, ADP_CS_4, &v) == 0 && (v & ADP_CS_4_LCK)) {
        Wr1(h, pr, SP_PORT, Port, ADP_CS_4, v & ~ADP_CS_4_LCK);
    }
    start = OsTimeMs();
    while ((k = Cfg(h, route, SP_SWITCH, 0, 0, cs, 1, FALSE, &up, 1000)) != 0) {
        if (OsTimeMs() - start > 1000) {
            UsbLog(LOG_ERR, "%s: no router behind %x:%u (%d)\n", h->Name, pr->Route, Port, k);
            return;
        }
        OsSleepMs(50);
    }
    if (Cfg(h, route, SP_SWITCH, 0, 0, cs, 5, FALSE, NULL, 1000) != 0) {
        return;
    }
    r = &h->R[ri];
    UsbMemSet(r, 0, sizeof(*r));
    r->Used = TRUE;
    r->Parent = (UCHAR)(Pi + 1);
    r->ParentPort = Port;
    r->Depth = (UCHAR)(pr->Depth + 1);
    r->Route = route;
    r->Up = up;
    r->Id = cs[0];
    r->MaxPort = (UCHAR)((cs[1] >> 14) & 0x3F);
    r->Version = (UCHAR)((cs[4] >> 29) & 7);
    pr->P[Port].Child = (UCHAR)(ri + 1);
    if (Port + 1 < TB_PORTS && pr->P[Port + 1].Lane1) {
        pr->P[Port + 1].Child = (UCHAR)(ri + 1);
    }

    /* route, depth, upstream adapter; enabled */
    cs[1] = (cs[1] & ~((0x3FUL << 8) | (7UL << 20))) | ((ULONG)up << 8) | ((ULONG)r->Depth << 20);
    cs[2] = route;
    cs[3] = 0x80000000UL;
    cs[4] = Cs4(r, cs[4]);
    if (Cfg(h, route, SP_SWITCH, 0, 1, &cs[1], 4, TRUE, NULL, 1000) != 0) {
        UsbLog(LOG_ERR, "%s: router %x could not be configured\n", h->Name, route);
        r->Used = FALSE;
        pr->P[Port].Child = 0;
        return;
    }
    ReadPorts(h, r);

    if (r->Version != 0) {
        ULONG cs5, cs6;
        BOOLEAN xhci;
        r->LinkUsb4 = pr->P[Port].CapUsb4 != 0 &&
                      Rd1(h, pr, SP_PORT, Port, pr->P[Port].CapUsb4 + PORT_CS_18, &v) == 0 && !(v & PORT_CS_18_TCM);
        Rd1(h, r, SP_SWITCH, 0, 6, &cs6);
        Rd1(h, r, SP_SWITCH, 0, 5, &cs5);
        xhci = (cs6 & CS_6_HCI) != 0;
        if (r->LinkUsb4 && FindType(pr, PT_USB3_DOWN) && OsGetConfig("TbtUsb3Tunnel", 1)) {
            cs5 |= CS_5_UTO;
            xhci = FALSE;
        }
        if (FindType(pr, PT_PCIE_DOWN)) {
            cs5 |= CS_5_PTO;
            if (xhci) {
                cs5 |= CS_5_HCO;
            }
        }
        cs5 &= ~CS_5_CNS;
        Wr1(h, r, SP_SWITCH, 0, 5, cs5);
        if (!WaitBits(h, r, SP_SWITCH, 0, 6, CS_6_RR, CS_6_RR, 500)) {
            UsbLog(LOG_ERR, "%s: router %x not ready\n", h->Name, route);
        }
        /* hot plug events of its downstream ports */
        for (i = 1; i <= r->MaxPort && i < TB_PORTS; i++) {
            if (r->P[i].CapUsb4 && Rd1(h, r, SP_PORT, (UCHAR)i, ADP_CS_5, &v) == 0 && (v & ADP_CS_5_DHP)) {
                Wr1(h, r, SP_PORT, (UCHAR)i, ADP_CS_5, v & ~ADP_CS_5_DHP);
            }
        }
        /* the link is configured, on both ends */
        if (r->P[up].CapUsb4 && Rd1(h, r, SP_PORT, up, r->P[up].CapUsb4 + PORT_CS_19, &v) == 0) {
            Wr1(h, r, SP_PORT, up, r->P[up].CapUsb4 + PORT_CS_19, v | PORT_CS_19_PC);
        }
    } else {
        /* Thunderbolt 1 / 2 / 3 device router: plug events on, lane configured */
        PlugEventsOn(h, r);
        Tb3LinkConfigured(h, r, up, TRUE);
    }
    if (pr->P[Port].CapUsb4 && Rd1(h, pr, SP_PORT, Port, pr->P[Port].CapUsb4 + PORT_CS_19, &v) == 0) {
        Wr1(h, pr, SP_PORT, Port, pr->P[Port].CapUsb4 + PORT_CS_19, v | PORT_CS_19_PC);
    } else if (pr->Version == 0) {
        Tb3LinkConfigured(h, pr, Port, FALSE);
    }
    Bond(h, pr, r);
    if (r->Version != 0) {
        Rd1(h, r, SP_SWITCH, 0, 5, &v);
        Wr1(h, r, SP_SWITCH, 0, 5, v | CS_5_CV);
        if (!WaitBits(h, r, SP_SWITCH, 0, 6, CS_6_CR, CS_6_CR, 500)) {
            UsbLog(LOG_ERR, "%s: router %x did not take its configuration\n", h->Name, route);
        }
    }
    UsbLog(LOG_INFO, "%s: router %04x:%04x at %x (depth %u, %s link, upstream adapter %u), %s\n", h->Name,
           r->Id & 0xFFFF, r->Id >> 16, route, r->Depth, r->LinkUsb4 ? "USB4" : "Thunderbolt", up, RouterKind(r));

    TunnelUsb3(h, pr, r);
    if (OsGetConfig("TbtApprove", 1) != 0) {
        TunnelPcie(h, pr, r);
    } else {
        UsbLog(LOG_INFO, "%s: no PCIe tunnel (TbtApprove = 0)\n", h->Name);
    }
    UsbNotifyChange();
    ScanRouter(h, ri);
}

/* a router went away: its tunnels end at the parent, which is still there */
static void Remove(TB_HOST *h, int Ri, BOOLEAN ParentThere)
{
    TB_ROUTER *r = &h->R[Ri];
    TB_ROUTER *pr = &h->R[r->Parent - 1];
    int p;

    for (p = 1; p <= r->MaxPort && p < TB_PORTS; p++) {
        if (r->P[p].Child && !r->P[p].Lane1) {
            Remove(h, r->P[p].Child - 1, FALSE);
        }
    }
    if (ParentThere) {
        UCHAR other = OtherLane(pr, r->ParentPort);
        ULONG v;
        if (pr->P[r->ParentPort].Bonded && !r->HwBonded && Rd1(h, pr, SP_PORT, r->ParentPort, pr->P[r->ParentPort].CapPhy + 1, &v) == 0) {
            /* bonding off again, single lane width on both lanes for the next device */
            Wr1(h, pr, SP_PORT, r->ParentPort, pr->P[r->ParentPort].CapPhy + 1, (v & ~(LANE_LB | 0x30UL)) | 0x10);
            if (other) {
                SetWidth(h, pr, other, 1);
            }
        }
        if (r->PcieDown) {
            Wr1(h, pr, SP_PORT, r->PcieDown, pr->P[r->PcieDown].CapAdap, 0);
            if (r->PcieEe && Rd1(h, pr, SP_PORT, r->PcieDown, pr->P[r->PcieDown].CapAdap + 1, &v) == 0) {
                Wr1(h, pr, SP_PORT, r->PcieDown, pr->P[r->PcieDown].CapAdap + 1, v & ~PCIE_CS_1_EE);
            }
            ClearHop(h, pr, r->PcieDown, PCIE_HOPID);
            ClearHop(h, pr, r->ParentPort, r->PcieLU);
        }
        if (r->Usb3Down) {
            Wr1(h, pr, SP_PORT, r->Usb3Down, pr->P[r->Usb3Down].CapAdap, USB3_V);
            ClearHop(h, pr, r->Usb3Down, USB3_HOPID);
            ClearHop(h, pr, r->ParentPort, r->Usb3LU);
            if (pr->Depth == 0) {
                Usb3Bandwidth(h, pr, r->Usb3Down, 0);
            }
        }
    }
    pr->P[r->ParentPort].HopOut &= ~((1ULL << r->PcieLD) | (1ULL << r->Usb3LD));
    pr->P[r->ParentPort].HopIn &= ~((1ULL << r->PcieLU) | (1ULL << r->Usb3LU));
    pr->P[r->ParentPort].Child = 0;
    pr->P[r->ParentPort].Bonded = FALSE;
    if (OtherLane(pr, r->ParentPort)) {
        pr->P[OtherLane(pr, r->ParentPort)].Bonded = FALSE;
    }
    if (r->ParentPort + 1 < TB_PORTS && pr->P[r->ParentPort + 1].Lane1) {
        pr->P[r->ParentPort + 1].Child = 0;
    }
    UsbLog(LOG_INFO, "%s: router %04x:%04x at %x removed\n", h->Name, r->Id & 0xFFFF, r->Id >> 16, r->Route);
    r->Used = FALSE;
    UsbNotifyChange();
}

static void PlugEvent(TB_HOST *h, TB_PKT *p)
{
    ULONG route = p->D[1];
    UCHAR port = (UCHAR)(p->D[2] & 0x3F);
    BOOLEAN unplug = (p->D[2] & 0x80000000UL) != 0;
    int i;

    for (i = 0; i < TB_ROUTERS; i++) {
        TB_ROUTER *r = &h->R[i];
        if (!r->Used || r->Route != route || (p->D[0] & 0x3FFFFF) != 0 || port > r->MaxPort || port >= TB_PORTS) {
            continue;
        }
        if (r->P[port].Type != PT_LANE || r->P[port].Lane1 || (i != 0 && port == r->Up)) {
            return;
        }
        UsbLog(LOG_INFO, "%s: %s at %x:%u\n", h->Name, unplug ? "unplugged" : "plugged", route, port);
        if (unplug) {
            if (r->P[port].Child) {
                Remove(h, r->P[port].Child - 1, TRUE);
            }
        } else if (!r->P[port].Child) {
            ULONG start = OsTimeMs();
            int st;
            while ((st = PortState(h, r, port)) != PORT_STATE_UP && OsTimeMs() - start < 1000) {
                OsSleepMs(20);
            }
            if (st == PORT_STATE_UP) {
                Enumerate(h, i, port);
            } else {
                UsbLog(LOG_INFO, "%s: link at %x:%u did not come up (%d)\n", h->Name, route, port, st);
            }
        }
        return;
    }
}

static BOOLEAN SwStart(TB_HOST *h)
{
    TB_ROUTER *r = &h->R[0];
    ULONG cs[5], v, caps = Rd(h, NHI_CAPS);
    UCHAR up = 0;
    int i;

    /* USB4 v2 host routers are reset first */
    if (((caps >> 16) & 0xFF) >= 0x40) {
        Wr(h, NHI_RESET, 1);
        OsSleepMs(100);
        for (i = 0; i < 50 && (Rd(h, NHI_RESET) & 1); i++) {
            OsSleepMs(10);
        }
        RingsStart(h);
    }
    if (Cfg(h, 0, SP_SWITCH, 0, 0, cs, 5, FALSE, &up, 1000) != 0) {
        UsbLog(LOG_ERR, "%s: host router does not answer\n", h->Name);
        return FALSE;
    }
    UsbMemSet(r, 0, sizeof(*r));
    r->Used = TRUE;
    r->Id = cs[0];
    r->Up = up;
    r->MaxPort = (UCHAR)((cs[1] >> 14) & 0x3F);
    r->Version = (UCHAR)((cs[4] >> 29) & 7);
    cs[1] = (cs[1] & ~((0x3FUL << 8) | (7UL << 20))) | ((ULONG)up << 8);
    cs[2] = 0;
    cs[3] = 0x80000000UL;
    cs[4] = Cs4(r, cs[4]);
    if (Cfg(h, 0, SP_SWITCH, 0, 1, &cs[1], 4, TRUE, NULL, 1000) != 0) {
        UsbLog(LOG_ERR, "%s: host router could not be configured\n", h->Name);
        return FALSE;
    }
    ReadPorts(h, r);
    if (r->Version == 0) {
        PlugEventsOn(h, r);                     /* Thunderbolt 1 / 2 / 3 host router (Apple) */
    }
    for (i = 1; i <= r->MaxPort && i < TB_PORTS; i++) {
        if (r->P[i].CapUsb4 && Rd1(h, r, SP_PORT, (UCHAR)i, ADP_CS_5, &v) == 0 && (v & ADP_CS_5_DHP)) {
            Wr1(h, r, SP_PORT, (UCHAR)i, ADP_CS_5, v & ~ADP_CS_5_DHP);
        }
    }
    {
        int lanes = 0, pcie = 0, usb3 = 0;
        for (i = 1; i <= r->MaxPort && i < TB_PORTS; i++) {
            lanes += r->P[i].Type == PT_LANE && !r->P[i].Lane1;
            pcie += r->P[i].Type == PT_PCIE_DOWN;
            usb3 += r->P[i].Type == PT_USB3_DOWN;
        }
        UsbLog(LOG_INFO, "%s: %s host router %04x:%04x, %u %s ports, %u PCIe and %u USB 3 adapters; the "
               "driver is the connection manager\n", h->Name,
               r->Version >= 2 ? "USB4 v2 (Thunderbolt 5)" : r->Version ? "USB4" : RouterGen(r) == 1 ? "Thunderbolt 1" :
               RouterGen(r) == 2 ? "Thunderbolt 2" : "Thunderbolt 3", r->Id & 0xFFFF, r->Id >> 16, lanes,
               r->Version ? "USB4" : "Thunderbolt", pcie, usb3);
    }
    ScanRouter(h, 0);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* start and service                                                    */
/* ------------------------------------------------------------------ */

/* Intel host interfaces (NHI device ids); mkinf.py makes the INF entries from this list */
static UCHAR Generation(USHORT Id)
{
    switch (Id) {
    case 0x1513: case 0x1547: case 0x1548: case 0x1566: case 0x1568: case 0x157D:
        return GEN_LEGACY;
    case 0x156A: case 0x156C:
        return GEN_FR;
    case 0x1575: case 0x1577: case 0x15BF: case 0x15D2: case 0x15D9:
        return GEN_AR;
    case 0x15DC: case 0x15DD: case 0x15DE:
        return GEN_USBONLY;
    case 0x15E8: case 0x15EB: case 0x1134: case 0x1137:
        return GEN_TR;
    case 0x8A17: case 0x8A0D: case 0x9A1B: case 0x9A1D: case 0x9A1F: case 0x9A21: case 0x463E: case 0x466D:
    case 0xA73E: case 0xA76D: case 0x7EB2: case 0x7EC2: case 0x7EC3:
        return GEN_ICL;
    case 0xA833: case 0xA834: case 0xE333: case 0xE334: case 0xE433: case 0xE434: case 0x4D33:
    case 0x5781: case 0x5784:
        return GEN_USB4;
    default:
        return 0;
    }
}

/* host interfaces with the device id of their own PCIe bridges: only the one of class 0880 (system
   peripheral) is the NHI (mkinf.py names them PCI\VEN_8086&DEV_xxxx&CC_0880) */
static const USHORT SharedIds[] = { 0x1513, 0x1547, 0x1548 };

/* what the controller is (usbtree, the log, the INF names; at most 35 characters) */
static const char *ChipName(USHORT Ven, USHORT Id)
{
    if (Ven == 0x1022) {
        return "USB4 (AMD)";
    }
    if (Ven != 0x8086) {
        return "USB4";
    }
    switch (Id) {
    case 0x1513: return "Thunderbolt 1 (Light Ridge)";
    case 0x1547: case 0x1548: return "Thunderbolt 1 (Cactus Ridge)";
    case 0x1566: case 0x1568: return "Thunderbolt 2 (Redwood Ridge)";
    case 0x157D: return "Thunderbolt 2 (Win Ridge)";
    case 0x156A: case 0x156C: return "Thunderbolt 2 (Falcon Ridge)";
    case 0x1575: case 0x1577: case 0x15BF: case 0x15D2: case 0x15D9: return "Thunderbolt 3 (Alpine Ridge)";
    case 0x15DC: case 0x15DD: case 0x15DE: return "Alpine Ridge (USB only mode)";
    case 0x15E8: case 0x15EB: return "Thunderbolt 3 (Titan Ridge)";
    case 0x1134: case 0x1137: return "Thunderbolt 4 (Maple Ridge)";
    case 0x8A17: case 0x8A0D: return "Thunderbolt 3 / USB4 (Ice Lake)";
    case 0x9A1B: case 0x9A1D: case 0x9A1F: case 0x9A21: return "Thunderbolt 4 / USB4 (Tiger Lake)";
    case 0x463E: case 0x466D: return "Thunderbolt 4 / USB4 (Alder Lake)";
    case 0xA73E: case 0xA76D: return "Thunderbolt 4 / USB4 (Raptor Lake)";
    case 0x7EB2: case 0x7EC2: case 0x7EC3: return "Thunderbolt 4 / USB4 (Meteor Lake)";
    case 0xA833: case 0xA834: return "Thunderbolt 4 / USB4 (Lunar Lake)";
    case 0xE333: case 0xE334: case 0xE433: case 0xE434: return "Thunderbolt 4 / USB4 (Panther Lake)";
    case 0x4D33: return "Thunderbolt 4 / USB4 (Wildcat Lake)";
    case 0x5781: return "Thunderbolt 5 (Barlow Ridge 80G)";
    case 0x5784: return "Thunderbolt 5 (Barlow Ridge 40G)";
    default: return "USB4";
    }
}

/* Apple puts its subsystem ids on the Thunderbolt controllers of the Macs (Linux: 2222:1111) */
static BOOLEAN IsApple(TB_HOST *h)
{
    ULONG ss = OsPciRead(h->Bus, h->Dev, h->Fn, 0x2C, 4);

    return (ss & 0xFFFF) == 0x106B || ss == 0x11112222UL;
}

static BOOLEAN IsIcl(USHORT Id)
{
    return Id == 0x8A17 || Id == 0x8A0D;
}

void TbtProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id, ULONG Cls)
{
    TB_HOST *h;
    ULONG bar, i;
    UCHAR gen = 0;

    if ((Id & 0xFFFF) == 0x8086) {
        gen = Generation((USHORT)(Id >> 16));
        for (i = 0; i < sizeof(SharedIds) / sizeof(SharedIds[0]); i++) {
            if ((USHORT)(Id >> 16) == SharedIds[i] && (Cls >> 8) != 0x0880) {
                gen = 0;                        /* one of its bridges */
            }
        }
    }
    if (gen == 0 && Cls != 0x0C0340) {
        return;
    }
    if (OsGetConfig("DisableTbt", 0) || TbtHostCount >= TB_HOSTS) {
        return;
    }
    h = (TB_HOST *)OsAlloc(sizeof(TB_HOST));
    if (h == NULL) {
        return;
    }
    h->Bus = Bus;
    h->Dev = Dev;
    h->Fn = Fn;
    h->DevId = (USHORT)(Id >> 16);
    h->Cls = Cls;
    h->Gen = gen ? gen : GEN_USB4;
    UsbFormat(h->Name, sizeof(h->Name), "thunderbolt %02x:%02x.%u", Bus, Dev, Fn);
    bar = OsPciRead(Bus, Dev, Fn, 0x10, 4);
    if (((bar >> 1) & 3) == 2 && OsPciRead(Bus, Dev, Fn, 0x14, 4) != 0) {
        bar = UsbPciRelocateBar(Bus, Dev, Fn, 0x10);
    }
    if ((bar & 1) || (bar & ~0xFUL) == 0) {
        UsbLog(LOG_ERR, "%s: registers not usable (BAR0 %08x)\n", h->Name, bar);
        OsFree(h);
        return;
    }
    h->Phys = bar & ~0xFUL;
    h->Regs = (volatile UCHAR *)OsMapMmio(Bus, h->Phys, NHI_LEN);
    h->Tx = (volatile TB_DESC *)OsDmaAlloc(4096, &h->DescPhys);
    h->Rx = h->Tx + TB_RING;
    h->TxBuf = (UCHAR *)OsDmaAlloc(2 * TB_RING * TB_FRAME, &h->TxBufPhys);
    if (h->Regs == NULL || h->Tx == NULL || h->TxBuf == NULL) {
        UsbLog(LOG_ERR, "%s: no memory\n", h->Name);
        if (h->Tx) OsDmaFree((void *)h->Tx, 4096);
        if (h->TxBuf) OsDmaFree(h->TxBuf, 2 * TB_RING * TB_FRAME);
        if (h->Regs) OsUnmapMmio((void *)h->Regs, NHI_LEN);
        OsFree(h);
        return;
    }
    h->RxBuf = h->TxBuf + TB_RING * TB_FRAME;
    h->RxBufPhys = h->TxBufPhys + TB_RING * TB_FRAME;
    /* memory and bus master on, legacy interrupt off */
    OsPciWrite(Bus, Dev, Fn, 4, OsPciRead(Bus, Dev, Fn, 4, 2) | 0x0406, 2);
    h->Used = TRUE;
    h->State = HS_START;
    for (i = 0; i < TB_HOSTS; i++) {
        if (Hosts[i] == NULL) {
            Hosts[i] = h;
            break;
        }
    }
    TbtHostCount++;
    UsbLog(LOG_INFO, "%s: %s host interface %04x:%04x\n", h->Name, ChipName((USHORT)Id, h->DevId), Id & 0xFFFF,
           Id >> 16);
}

static void SetState(TB_HOST *h, UCHAR S)
{
    h->State = S;
    h->StateAt = OsTimeMs();
    h->Tries = 0;
}

static void DriverReady(TB_HOST *h)
{
    ULONG m[1];

    m[0] = ICM_DRIVER_READY;
    if (h->Gen == GEN_TR) {
        IcmSend(h, -1, m, 1, 250, 10);
    } else if (h->Gen == GEN_ICL) {
        IcmSend(h, -1, m, 1, 20000, 3);
    } else {
        IcmSend(h, -1, m, 1, 5000, 3);
    }
    SetState(h, HS_DRVREADY);
}

static void Start(TB_HOST *h)
{
    ULONG v, i;
    BOOLEAN icm = FALSE;

    v = Rd(h, NHI_CAPS);
    if (v == 0xFFFFFFFFUL) {
        UsbLog(LOG_INFO, "%s: controller is powered off (nothing connected; the BIOS powers it up)\n", h->Name);
        SetState(h, HS_DEAD);
        return;
    }
    if (h->Gen == GEN_USBONLY) {
        UsbLog(LOG_INFO, "%s: Thunderbolt is switched off (USB only mode, BIOS setup): its USB controller works "
               "without a connection manager\n", h->Name);
        SetState(h, HS_BIOS);
        return;
    }
    if (h->Gen == GEN_LEGACY) {
        /* PCs: the BIOS does it (SMI); a second connection manager would fight it.  Macs: the OS does it */
        ULONG cm = OsGetConfig("TbtDriverCm", 2);
        if (cm == 0 || (cm == 2 && !IsApple(h))) {
            UsbLog(LOG_INFO, "%s: Thunderbolt 1 / 2 controller of a PC: the BIOS manages it (devices come through "
                   "PCIe hot plug); TbtDriverCm=1 lets the driver do it\n", h->Name);
            SetState(h, HS_BIOS);
            return;
        }
        h->HopCount = v & 0x3FF;
        RingsStart(h);
        h->Gen = GEN_USB4;
        SetState(h, HS_SW);
        if (!SwStart(h)) {
            SetState(h, HS_DEAD);
        }
        return;
    }
    h->HopCount = v & 0x3FF;
    if (h->Gen == GEN_ICL && IsIcl(h->DevId)) {
        /* Ice Lake: force power, then wait for the firmware */
        v = OsPciRead(h->Bus, h->Dev, h->Fn, VS_CAP_22, 4);
        v = (v & 0x00FFFFFFUL) | (0x22UL << 24) | 0x02;
        OsPciWrite(h->Bus, h->Dev, h->Fn, VS_CAP_22, v, 4);
        SetState(h, HS_POWER);
        return;
    }
    RingsStart(h);
    if (h->Gen == GEN_ICL || h->DevId == 0x1134 || h->DevId == 0x1137) {
        /* Tiger Lake and later: firmware runs only when the BIOS chose it */
        for (i = 0; i < 10 && !icm; i++) {
            icm = (Rd(h, NHI_FW_STS) & FW_STS_NVM_AUTH_DONE) != 0;
            if (!icm) {
                OsSleepMs(1);
            }
        }
    } else if (h->Gen != GEN_USB4) {
        icm = (Rd(h, NHI_FW_STS) & FW_STS_ICM_EN) != 0;
        if (!icm) {
            if (OsGetConfig("TbtDriverCm", 2) == 0) {
                UsbLog(LOG_ERR, "%s: the firmware connection manager is not running, Thunderbolt not used "
                       "(TbtDriverCm = 0)\n", h->Name);
                SetState(h, HS_DEAD);
                return;
            }
            UsbLog(LOG_INFO, "%s: the firmware connection manager is not running (%s): the driver does it\n",
                   h->Name, IsApple(h) ? "Apple Mac" : "switched off");
        }
    }
    if (!icm) {
        h->Gen = GEN_USB4;
        SetState(h, HS_SW);
        if (!SwStart(h)) {
            SetState(h, HS_DEAD);
        }
        return;
    }
    if (h->Gen == GEN_AR || h->Gen == GEN_TR) {
        ULONG mode;
        for (i = 0; i < 60 && !(Rd(h, NHI_FW_STS) & FW_STS_NVM_AUTH_DONE); i++) {
            OsSleepMs(50);
        }
        if (!(Rd(h, NHI_FW_STS) & FW_STS_NVM_AUTH_DONE)) {
            UsbLog(LOG_ERR, "%s: firmware not authenticated\n", h->Name);
            SetState(h, HS_DEAD);
            return;
        }
        mode = (Rd(h, NHI_OUTMAIL_CMD) >> 8) & 0xF;
        if (mode == FW_MODE_SAFE) {
            UsbLog(LOG_ERR, "%s: controller firmware is in safe mode (needs an NVM update)\n", h->Name);
            SetState(h, HS_DEAD);
            return;
        }
        if (mode == FW_MODE_CM) {
            Wr(h, NHI_INMAIL_DATA, 0);
            v = Rd(h, NHI_INMAIL_CMD) & ~(0xFFUL | INMAIL_ERROR);
            Wr(h, NHI_INMAIL_CMD, v | INMAIL_OP_REQUEST | MAIL_ALLOW_ALL_DEVS);
            for (i = 0; i < 500 && (Rd(h, NHI_INMAIL_CMD) & INMAIL_OP_REQUEST); i++) {
                OsStallUs(20);
            }
        } else {
            UsbLog(LOG_ERR, "%s: firmware in mode %u\n", h->Name, mode);
            SetState(h, HS_DEAD);
            return;
        }
    }
    DriverReady(h);
}

static void IcmService(TB_HOST *h)
{
    int i;

    /* answer to the request in flight */
    if (h->IcmBusy) {
        if (h->IcmDone) {
            if (h->State == HS_DRVREADY) {
                const UCHAR *b = (const UCHAR *)h->IcmResp.D;
                h->IcmBusy = FALSE;
                switch (h->Gen) {
                case GEN_FR:
                    h->Sl = (UCHAR)(b[6] & 0xF);
                    break;
                case GEN_AR:
                    h->Sl = (UCHAR)(b[6] & 0xF);
                    break;
                case GEN_TR:
                    h->Sl = (UCHAR)(b[6] & 0x7);
                    break;
                default:
                    h->Sl = 0;
                    break;
                }
                if (h->Sl > 5) {
                    h->Sl = 5;
                }
                SetState(h, HS_ROOT);
            } else {
                IcmReply(h);
            }
        } else if (OsTimeMs() - h->IcmSent > h->IcmTimeout) {
            if (--h->IcmTries > 0) {
                h->IcmSent = OsTimeMs();
                Tx(h, PKG_ICM_CMD, h->IcmReq, h->IcmReqDw);
            } else {
                h->IcmBusy = FALSE;
                if (h->State == HS_DRVREADY) {
                    UsbLog(LOG_ERR, "%s: firmware does not answer \"driver ready\"\n", h->Name);
                    SetState(h, HS_DEAD);
                    return;
                }
                UsbLog(LOG_ERR, "%s: firmware request %02x timed out\n", h->Name, h->IcmCode);
                if (h->IcmDev >= 0) {
                    h->D[h->IcmDev].State = DS_REFUSED;
                    UsbFormat(h->D[h->IcmDev].Note, sizeof(h->D[0].Note), "firmware timeout");
                }
            }
        }
    }
    if (h->State == HS_ROOT) {
        ULONG v;
        if (Cfg(h, 0, SP_SWITCH, 0, 0, &v, 1, FALSE, NULL, 100) == 0) {
            UsbLog(LOG_INFO, "%s: firmware connection manager ready (host router %04x:%04x, security level %s)\n",
                   h->Name, v & 0xFFFF, v >> 16, h->Gen == GEN_ICL ? "set by the BIOS" : SlName[h->Sl]);
            SetState(h, HS_ICM);
            UsbNotifyChange();
        } else if (++h->Tries > 50) {
            UsbLog(LOG_ERR, "%s: host router not reachable after \"driver ready\"\n", h->Name);
            SetState(h, HS_DEAD);
        }
        return;
    }
    if (h->State != HS_ICM) {
        return;
    }
    while (h->EvCount > 0) {
        TB_PKT *p = &h->Ev[h->EvHead];
        h->EvHead = (h->EvHead + 1) % TB_EVQ;
        h->EvCount--;
        if (p->Type != PKG_ICM_EVENT) {
            continue;
        }
        switch (p->D[0] & 0xFF) {
        case ICM_EV_CONNECTED:
            IcmConnected(h, p);
            UsbNotifyChange();
            break;
        case ICM_EV_DISCONNECTED:
            IcmDisconnected(h, p);
            break;
        default:
            break;
        }
    }
    if (!h->IcmBusy) {
        for (i = 0; i < TB_DEVS; i++) {
            UCHAR s = h->D[i].State;
            if (h->D[i].Used && (s == DS_APPROVE || s == DS_ADDKEY || s == DS_CHALLENGE)) {
                IcmNext(h, i);
                break;
            }
        }
    }
}

static void Service(TB_HOST *h)
{
    ULONG start;

    switch (h->State) {
    case HS_START:
        Start(h);
        return;
    case HS_POWER:
        if (OsPciRead(h->Bus, h->Dev, h->Fn, VS_CAP_9, 4) & 0x80000000UL) {
            ULONG ltr = OsPciRead(h->Bus, h->Dev, h->Fn, VS_CAP_16, 4) & 0xFFFF;
            OsPciWrite(h->Bus, h->Dev, h->Fn, VS_CAP_15, (ltr << 16) | ltr, 4);
            RingsStart(h);
            DriverReady(h);
        } else if (OsTimeMs() - h->StateAt > 1100) {
            UsbLog(LOG_ERR, "%s: firmware did not come up after force power\n", h->Name);
            SetState(h, HS_DEAD);
        }
        return;
    case HS_DEAD:
        return;
    default:
        break;
    }
    if (HostGone(h)) {
        UsbLog(LOG_ERR, "%s: controller gone\n", h->Name);
        SetState(h, HS_DEAD);
        return;
    }
    /* a little time for answers that are on the way */
    start = OsTimeMs();
    do {
        Poll(h);
        if (!h->IcmBusy || h->IcmDone) {
            break;
        }
        OsStallUs(100);
    } while (OsTimeMs() - start < 20);
    if (h->State == HS_SW) {
        while (h->EvCount > 0) {
            TB_PKT p;
            UsbMemCpy(&p, &h->Ev[h->EvHead], sizeof(p));
            h->EvHead = (h->EvHead + 1) % TB_EVQ;
            h->EvCount--;
            if (p.Type == PKG_EVENT) {
                PlugEvent(h, &p);
            }
        }
    } else {
        IcmService(h);
    }
}

void TbtService(void)
{
    int i;

    for (i = 0; i < TB_HOSTS; i++) {
        if (Hosts[i] != NULL) {
            Service(Hosts[i]);
        }
    }
}

void TbtShutdown(void)
{
    int i;

    for (i = 0; i < TB_HOSTS; i++) {
        TB_HOST *h = Hosts[i];
        if (h != NULL && h->State != HS_DEAD && h->State != HS_START && !HostGone(h)) {
            RingsStop(h);
            if (h->Gen == GEN_ICL && IsIcl(h->DevId)) {
                ULONG v = OsPciRead(h->Bus, h->Dev, h->Fn, VS_CAP_22, 4);
                OsPciWrite(h->Bus, h->Dev, h->Fn, VS_CAP_22, v & ~0x02UL, 4);
            }
        }
    }
}

/* usbtree: host routers, then their devices (firmware) or routers (driver);
   Detail lines are separated by '\n' */
BOOLEAN TbtInfo(int Index, char *Name, int Size, char *Detail, int DSize)
{
    int i, k;

    for (i = 0; i < TB_HOSTS; i++) {
        TB_HOST *h = Hosts[i];
        if (h == NULL) {
            continue;
        }
        if (Index-- == 0) {
            const char *chip = ChipName((USHORT)OsPciRead(h->Bus, h->Dev, h->Fn, 0, 2), h->DevId);
            if (h->State == HS_SW && h->R[0].Used && h->R[0].Version >= 2 && chip[0] == 'U') {
                chip = "USB4 v2 (80 Gb/s)";
            }
            UsbFormat(Name, Size, "%s host router", chip);
            UsbFormat(Detail, DSize, "PCI %02x:%02x.%u, id %04x\nConnection manager: %s%s%s", h->Bus, h->Dev, h->Fn,
                      h->DevId,
                      h->State == HS_DEAD ? "none (not in use, see the log)" :
                      h->State == HS_BIOS ? "the BIOS / not needed (see the log)" :
                      h->State == HS_SW ? "the driver" :
                      h->State == HS_ICM ? "firmware" : "starting",
                      h->State == HS_ICM && h->Gen != GEN_ICL ? "\nSecurity level: " : "",
                      h->State == HS_ICM && h->Gen != GEN_ICL ? SlName[h->Sl] : "");
            return TRUE;
        }
        for (k = 0; k < TB_DEVS; k++) {
            TB_DEV *d = &h->D[k];
            if (d->Used && Index-- == 0) {
                char u[40];
                UuidStr(d->Uuid, u);
                UsbFormat(Name, Size, "Device %04x:%04x: %s", d->Vendor, d->Model,
                          d->Note[0] ? d->Note : "connecting");
                UsbFormat(Detail, DSize, "UUID %s\nLink %u, depth %u, %u Gb/s\nSecurity level: %s%s", u, d->LinkInfo & 7,
                          (d->LinkInfo >> 4) & 15, d->Gbps, SlName[d->Sl], d->HaveKey ? ", key stored" : "");
                return TRUE;
            }
        }
        for (k = 1; k < TB_ROUTERS; k++) {
            TB_ROUTER *r = &h->R[k];
            if (r->Used && Index-- == 0) {
                UsbFormat(Name, Size, "%s %04x:%04x - %s%s", RouterKind(r), r->Id & 0xFFFF, r->Id >> 16,
                          r->PcieDown ? "PCIe" : "no PCIe", r->Usb3Down ? " + USB 3" : "");
                UsbFormat(Detail, DSize, "Route %x, depth %u, upstream adapter %u\n%s link, %u Gb/s (%u x %u)%s%s%s",
                          r->Route, r->Depth, r->Up, r->LinkUsb4 ? (r->LinkGbps >= 40 ? "Thunderbolt 5 / USB4 v2 Gen 4" : "USB4") :
                          "Thunderbolt", r->LinkGbps * r->LinkLanes, r->LinkLanes, r->LinkGbps,
                          r->PcieDown ? "\nPCIe tunnel" : "", r->PcieEe ? " (extended encapsulation)" : "",
                          r->Usb3Down ? "\nUSB 3 tunnel" : "");
                return TRUE;
            }
        }
    }
    return FALSE;
}
