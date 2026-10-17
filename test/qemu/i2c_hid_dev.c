/*
 * HID over I2C test device (for the USBNT HID over I2C driver).
 *
 *   -device i2c-hid,bus=i2c0.0,address=0x2c,kind=ptp[,desc-reg=0x20][,repeat=on][,display=vga2]
 *
 * kinds:
 *   mouse   3 buttons, wheel, relative (report id 1)
 *   ptp     precision touch pad: mouse collection (report id 2) that is used
 *           in the default mouse mode, and a touch pad collection (id 3)
 *   tpad    touch pad without a mouse collection: one finger, absolute
 *           (abs events move the finger, the left button puts it down)
 *   touch   touch screen: absolute, tip switch (left button), no report ids
 *   kbd     keyboard (report id 1) with a LED output report
 *
 * Protocol: HID descriptor at desc-reg; report descriptor, input, output,
 * command and data registers; SET_POWER, RESET (zero length report after
 * it), GET_REPORT / SET_REPORT through the data register. A read with no
 * register write first returns the next queued input report, or a zero
 * length report when there is none (repeat=on: the last report again, like
 * some firmware).
 *
 * Properties for tests: "reports" (input reports read by the host), "leds"
 * (last LED output report), "resets", "power" (0 on, 1 sleep).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "ui/input.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"

#define TYPE_I2C_HID "i2c-hid"
OBJECT_DECLARE_SIMPLE_TYPE(I2cHidDev, I2C_HID)

#define REG_DESC_DEFAULT 0x20
#define REG_REPDESC 0x0100
#define REG_INPUT   0x0200
#define REG_OUTPUT  0x0300
#define REG_CMD     0x0400
#define REG_DATA    0x0500
#define QLEN        32
#define REPMAX      32

enum { K_MOUSE, K_PTP, K_TPAD, K_TOUCH, K_KBD };

struct I2cHidDev {
    I2CSlave parent_obj;
    char *kind_str;
    char *display;
    uint32_t head;
    uint16_t desc_reg;
    bool repeat;
    bool hybrid;                /* PTP: one contact per report */
    uint32_t reports, resets, power, leds, input_mode;
    uint32_t gate_port;         /* SystemIO power gate (0 = off): the device does not */
    bool gate_open;             /* answer its HID descriptor until _PS0 writes this port */
    MemoryRegion gate_io;
    int kind;

    const uint8_t *rdesc;
    unsigned rdesc_len;
    unsigned in_len;            /* max input length incl. the 2 length bytes */

    /* transfer state */
    uint8_t wbuf[64];
    unsigned wn;
    const uint8_t *src;
    unsigned src_len, src_pos;
    uint8_t rbuf[64];
    bool reset_pending;

    /* input queue */
    uint8_t q[QLEN][REPMAX];
    unsigned qn, qhead;
    uint8_t last[REPMAX];
    bool have_last;

    /* event accumulation */
    int dx, dy, wheel, buttons, absx, absy, fwas;
    bool moved;
    uint8_t keys[6];
    uint8_t mods;

    QemuInputHandlerState *hs;
};

static const uint8_t rd_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xc0, 0xc0,
};

/* precision touch pad: mouse collection (id 2, the default mouse mode), touch
   pad collection (id 3: two fingers with confidence, tip, contact id, X, Y;
   contact count; button), contact count maximum (feature id 4) and the
   configuration collection with Input Mode (feature id 5) */
#define FINGER \
    0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, \
    0x15, 0x00, 0x25, 0x01, 0x09, 0x47, 0x09, 0x42, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02, \
    0x95, 0x06, 0x81, 0x03, \
    0x25, 0x0f, 0x75, 0x08, 0x95, 0x01, 0x09, 0x51, 0x81, 0x02, \
    0x05, 0x01, 0x15, 0x00, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, \
    0xc0
static const uint8_t rd_ptp[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0,
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03,
    FINGER, FINGER,
    0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,       /* contact count */
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
    0x05, 0x0d, 0x85, 0x04, 0x09, 0x55, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02,  /* contact count max */
    0xc0,
    0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x05, 0x09, 0x22, 0xa1, 0x02,       /* configuration */
    0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0, 0xc0,
};

/* hybrid reporting: one contact slot per report */
static const uint8_t rd_ptp1[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0,
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03,
    FINGER,
    0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,       /* contact count */
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
    0x05, 0x0d, 0x85, 0x04, 0x09, 0x55, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02,  /* contact count max */
    0xc0,
    0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x05, 0x09, 0x22, 0xa1, 0x02,       /* configuration */
    0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0, 0xc0,
};

/* touch pad without a mouse collection */
static const uint8_t rd_tpad[] = {
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03,
    0x09, 0x22, 0xa1, 0x02,
    0x15, 0x00, 0x25, 0x01, 0x09, 0x47, 0x09, 0x42, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x15, 0x00, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
    0xc0,
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
    0xc0,
};

/* touch screen, no report ids */
static const uint8_t rd_touch[] = {
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01,
    0x09, 0x22, 0xa1, 0x02,
    0x15, 0x00, 0x25, 0x01, 0x09, 0x42, 0x09, 0x32, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x15, 0x00, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
    0xc0,
    0x05, 0x0d, 0x09, 0x54, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xc0,
};

static const uint8_t rd_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x81, 0x00,
    0xc0,
};

static void hid_queue(I2cHidDev *s, const uint8_t *rep, unsigned len)
{
    uint8_t *e;

    if (s->qn == QLEN || len + 2 > REPMAX) {
        return;
    }
    e = s->q[(s->qhead + s->qn) % QLEN];
    memset(e, 0, REPMAX);
    e[0] = len + 2;
    e[1] = 0;
    memcpy(e + 2, rep, len);
    s->qn++;
}

static void make_report(I2cHidDev *s)
{
    uint8_t r[24];
    int x, y;

    memset(r, 0, sizeof(r));
    switch (s->kind) {
    case K_MOUSE:
        while (s->dx || s->dy || s->wheel || s->moved) {
            int dx = MAX(-127, MIN(127, s->dx)), dy = MAX(-127, MIN(127, s->dy));
            int w = MAX(-127, MIN(127, s->wheel));
            r[0] = 1; r[1] = s->buttons & 7; r[2] = dx; r[3] = dy; r[4] = w;
            hid_queue(s, r, 5);
            s->dx -= dx; s->dy -= dy; s->wheel -= w;
            s->moved = false;
        }
        break;
    case K_PTP:
        if (s->input_mode == 3) {
            /* touch pad mode: finger 0 at the absolute position (left button =
               on the pad), finger 1 300 units to its right (right button), the
               middle button is the pad's click */
            int x0 = s->absx * 4095 / 0x7fff, y0 = s->absy * 4095 / 0x7fff;
            int on0 = s->buttons & 1, on1 = (s->buttons & 2) != 0, was0 = s->fwas & 1, was1 = (s->fwas & 2) != 0;
            int n = (on0 || was0) + (on1 || was1), k;
            uint8_t f[2][6];
            memset(f, 0, sizeof(f));
            f[0][0] = on0 ? 3 : 1; f[0][1] = 1; stw_le_p(&f[0][2], x0); stw_le_p(&f[0][4], y0);
            f[1][0] = on1 ? 3 : 1; f[1][1] = 2; stw_le_p(&f[1][2], MIN(x0 + 300, 4095)); stw_le_p(&f[1][4], y0);
            if (n == 0 && !(s->buttons & 4) && !(s->fwas & 4)) {
                break;
            }
            if (s->hybrid) {
                int first = 1;
                for (k = 0; k < 2; k++) {
                    if (!(k == 0 ? (on0 || was0) : (on1 || was1))) {
                        continue;
                    }
                    memset(r, 0, sizeof(r));
                    r[0] = 3;
                    memcpy(r + 1, f[k], 6);
                    r[7] = first ? n : 0;
                    r[8] = (s->buttons & 4) ? 1 : 0;
                    hid_queue(s, r, 9);
                    first = 0;
                }
                if (first) {                        /* only the button changed */
                    memset(r, 0, sizeof(r));
                    r[0] = 3;
                    r[8] = (s->buttons & 4) ? 1 : 0;
                    hid_queue(s, r, 9);
                }
            } else {
                int m = 0;
                memset(r, 0, sizeof(r));
                r[0] = 3;
                if (on0 || was0) { memcpy(r + 1 + 6 * m, f[0], 6); m++; }
                if (on1 || was1) { memcpy(r + 1 + 6 * m, f[1], 6); m++; }
                r[13] = m;
                r[14] = (s->buttons & 4) ? 1 : 0;
                hid_queue(s, r, 15);
            }
            s->fwas = s->buttons & 7;
            break;
        }
        while (s->dx || s->dy || s->moved) {
            int dx = MAX(-127, MIN(127, s->dx)), dy = MAX(-127, MIN(127, s->dy));
            r[0] = 2; r[1] = s->buttons & 3; r[2] = dx; r[3] = dy;
            hid_queue(s, r, 4);
            s->dx -= dx; s->dy -= dy;
            s->moved = false;
        }
        break;
    case K_TPAD:
        x = s->absx * 4095 / 0x7fff;
        y = s->absy * 4095 / 0x7fff;
        r[0] = 3; r[1] = (s->buttons & 1) ? 3 : 1;       /* confidence, tip */
        r[2] = x; r[3] = x >> 8; r[4] = y; r[5] = y >> 8;
        r[6] = (s->buttons & 2) ? 1 : 0;                  /* the pad button */
        hid_queue(s, r, 7);
        break;
    case K_TOUCH:
        x = s->absx * 4095 / 0x7fff;
        y = s->absy * 4095 / 0x7fff;
        r[0] = (s->buttons & 1) ? 3 : 2;                  /* tip, in range */
        r[1] = x; r[2] = x >> 8; r[3] = y; r[4] = y >> 8; r[5] = 1;
        hid_queue(s, r, 6);
        break;
    case K_KBD:
        r[0] = 1; r[1] = s->mods; r[2] = 0;
        memcpy(r + 3, s->keys, 6);
        hid_queue(s, r, 9);
        break;
    }
}

static void hid_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    I2cHidDev *s = I2C_HID(dev);
    InputMoveEvent *move;
    InputBtnEvent *btn;
    InputKeyEvent *key;
    int bit, i, code;

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        move = evt->u.rel.data;
        if (move->axis == INPUT_AXIS_X) {
            s->dx += move->value;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->dy += move->value;
        }
        break;
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            s->absx = move->value;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->absy = move->value;
        }
        s->moved = true;
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_WHEEL_UP && btn->down) {
            s->wheel++;
        } else if (btn->button == INPUT_BUTTON_WHEEL_DOWN && btn->down) {
            s->wheel--;
        }
        bit = btn->button == INPUT_BUTTON_LEFT ? 1 : btn->button == INPUT_BUTTON_RIGHT ? 2 :
              btn->button == INPUT_BUTTON_MIDDLE ? 4 : 0;
        if (bit) {
            s->buttons = btn->down ? (s->buttons | bit) : (s->buttons & ~bit);
        }
        s->moved = true;
        break;
    case INPUT_EVENT_KIND_KEY:
        key = evt->u.key.data;
        code = qemu_input_key_value_to_qcode(key->key);
        {
            /* a few keys are enough for the tests: letters, digits, shift, ctrl, enter, space */
            static const struct { int q; uint8_t u; } map[] = {
                { Q_KEY_CODE_A, 0x04 }, { Q_KEY_CODE_B, 0x05 }, { Q_KEY_CODE_C, 0x06 }, { Q_KEY_CODE_D, 0x07 },
                { Q_KEY_CODE_E, 0x08 }, { Q_KEY_CODE_F, 0x09 }, { Q_KEY_CODE_G, 0x0a }, { Q_KEY_CODE_H, 0x0b },
                { Q_KEY_CODE_I, 0x0c }, { Q_KEY_CODE_J, 0x0d }, { Q_KEY_CODE_K, 0x0e }, { Q_KEY_CODE_L, 0x0f },
                { Q_KEY_CODE_M, 0x10 }, { Q_KEY_CODE_N, 0x11 }, { Q_KEY_CODE_O, 0x12 }, { Q_KEY_CODE_P, 0x13 },
                { Q_KEY_CODE_Q, 0x14 }, { Q_KEY_CODE_R, 0x15 }, { Q_KEY_CODE_S, 0x16 }, { Q_KEY_CODE_T, 0x17 },
                { Q_KEY_CODE_U, 0x18 }, { Q_KEY_CODE_V, 0x19 }, { Q_KEY_CODE_W, 0x1a }, { Q_KEY_CODE_X, 0x1b },
                { Q_KEY_CODE_Y, 0x1c }, { Q_KEY_CODE_Z, 0x1d }, { Q_KEY_CODE_1, 0x1e }, { Q_KEY_CODE_2, 0x1f },
                { Q_KEY_CODE_RET, 0x28 }, { Q_KEY_CODE_SPC, 0x2c }, { Q_KEY_CODE_CAPS_LOCK, 0x39 },
                { Q_KEY_CODE_NUM_LOCK, 0x53 }, { Q_KEY_CODE_RIGHT, 0x4f }, { Q_KEY_CODE_KP_ENTER, 0x58 },
            };
            if (code == Q_KEY_CODE_SHIFT || code == Q_KEY_CODE_CTRL || code == Q_KEY_CODE_ALT) {
                int m = code == Q_KEY_CODE_CTRL ? 1 : code == Q_KEY_CODE_SHIFT ? 2 : 4;
                s->mods = key->down ? (s->mods | m) : (s->mods & ~m);
                break;
            }
            for (i = 0; i < ARRAY_SIZE(map); i++) {
                if (map[i].q == code) {
                    int j;
                    uint8_t u = map[i].u;
                    if (key->down) {
                        for (j = 0; j < 6 && s->keys[j] && s->keys[j] != u; j++) {
                        }
                        if (j < 6) {
                            s->keys[j] = u;
                        }
                    } else {
                        for (j = 0; j < 6; j++) {
                            if (s->keys[j] == u) {
                                memmove(s->keys + j, s->keys + j + 1, 5 - j);
                                s->keys[5] = 0;
                                break;
                            }
                        }
                    }
                }
            }
            s->moved = true;
        }
        break;
    default:
        break;
    }
}

static void hid_sync(DeviceState *dev)
{
    I2cHidDev *s = I2C_HID(dev);

    if (s->moved || s->dx || s->dy || s->wheel) {
        make_report(s);
        s->moved = false;
    }
}

static const QemuInputHandler hid_rel_handler = {
    .name = "I2C HID pointer",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = hid_event,
    .sync = hid_sync,
};

static const QemuInputHandler hid_ptp_handler = {
    .name = "I2C HID touch pad",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_ABS,
    .event = hid_event,
    .sync = hid_sync,
};

static const QemuInputHandler hid_abs_handler = {
    .name = "I2C HID touch",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = hid_event,
    .sync = hid_sync,
};

static const QemuInputHandler hid_kbd_handler = {
    .name = "I2C HID keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = hid_event,
    .sync = hid_sync,
};

static void hid_desc(I2cHidDev *s, uint8_t *d)
{
    memset(d, 0, 30);
    stw_le_p(d + 0, 30);
    stw_le_p(d + 2, 0x0100);
    stw_le_p(d + 4, s->rdesc_len);
    stw_le_p(d + 6, REG_REPDESC);
    stw_le_p(d + 8, REG_INPUT);
    stw_le_p(d + 10, s->in_len);
    stw_le_p(d + 12, REG_OUTPUT);
    stw_le_p(d + 14, 4);
    stw_le_p(d + 16, REG_CMD);
    stw_le_p(d + 18, REG_DATA);
    stw_le_p(d + 20, 0x06cb);
    stw_le_p(d + 22, 0x7000 + s->kind);
    stw_le_p(d + 24, 0x0105);
}

static void hid_input(I2cHidDev *s)
{
    memset(s->rbuf, 0, sizeof(s->rbuf));
    if (s->reset_pending) {
        s->reset_pending = false;
    } else if (s->qn) {
        memcpy(s->rbuf, s->q[s->qhead], REPMAX);
        memcpy(s->last, s->rbuf, REPMAX);
        s->have_last = true;
        s->qhead = (s->qhead + 1) % QLEN;
        s->qn--;
        s->reports++;
    } else if (s->repeat && s->have_last) {
        memcpy(s->rbuf, s->last, REPMAX);
    }
    s->src = s->rbuf;
    s->src_len = s->in_len;
    s->src_pos = 0;
}

static void hid_command(I2cHidDev *s)
{
    uint16_t reg = s->wbuf[0] | (s->wbuf[1] << 8);
    uint8_t low, op;

    if (s->wn < 2) {
        return;
    }
    if (reg == REG_OUTPUT && s->wn >= 5) {
        /* length, report id, LEDs */
        if (s->kind == K_KBD && s->wbuf[4] == 1 && s->wn >= 6) {
            s->leds = s->wbuf[5];
        }
        return;
    }
    if (reg != REG_CMD || s->wn < 4) {
        return;
    }
    low = s->wbuf[2];
    op = s->wbuf[3] & 0x0f;
    switch (op) {
    case 0x01:                  /* RESET */
        s->resets++;
        s->input_mode = 0;      /* back to mouse mode */
        s->fwas = 0;
        s->qn = 0;
        s->have_last = false;
        s->reset_pending = true;
        break;
    case 0x08:                  /* SET_POWER */
        s->power = low & 3;
        break;
    case 0x03:                  /* SET_REPORT: data register, length, id, data */
        if (s->kind == K_PTP && (low & 0x30) == 0x30 && (low & 0x0f) == 5 && s->wn >= 10 &&
            (s->wbuf[4] | (s->wbuf[5] << 8)) == REG_DATA && s->wbuf[8] == 5) {
            s->input_mode = s->wbuf[9];
            s->qn = 0;
        }
        if (s->kind == K_KBD && s->wn >= 10 && (s->wbuf[4] | (s->wbuf[5] << 8)) == REG_DATA) {
            s->leds = s->wbuf[9];
        }
        break;
    default:
        break;
    }
}

/* SystemIO power gate: a write of a non-zero value powers the device on so it
   answers its HID descriptor (notebook firmware does this in _PS0). */
static uint64_t gate_read(void *opaque, hwaddr addr, unsigned size)
{
    I2cHidDev *s = opaque;
    return s->gate_open ? 1 : 0;
}

static void gate_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    I2cHidDev *s = opaque;
    if (val) {
        s->gate_open = true;
    }
}

static const MemoryRegionOps gate_ops = {
    .read = gate_read,
    .write = gate_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static int hid_ev(I2CSlave *i2c, enum i2c_event event)
{
    I2cHidDev *s = I2C_HID(i2c);
    uint16_t reg;

    switch (event) {
    case I2C_START_SEND:
        s->wn = 0;
        s->src = NULL;
        break;
    case I2C_START_RECV:
        reg = s->wn >= 2 ? (s->wbuf[0] | (s->wbuf[1] << 8)) : 0xffff;
        if (s->wn >= 2 && reg == s->desc_reg) {
            hid_desc(s, s->rbuf);
            if (s->gate_port && !s->gate_open) {
                memset(s->rbuf, 0, 30);   /* powered off: an invalid descriptor */
            }
            s->src = s->rbuf;
            s->src_len = 30;
        } else if (s->wn >= 2 && reg == REG_REPDESC) {
            s->src = s->rdesc;
            s->src_len = s->rdesc_len;
        } else if (s->wn >= 2 && reg == REG_INPUT) {
            hid_input(s);
        } else if (s->wn >= 2) {
            /* anything else reads as zeros */
            memset(s->rbuf, 0, sizeof(s->rbuf));
            s->src = s->rbuf;
            s->src_len = sizeof(s->rbuf);
        } else {
            hid_input(s);
        }
        s->src_pos = 0;
        s->wn = 0;
        break;
    case I2C_FINISH:
        if (s->wn) {
            hid_command(s);
        }
        s->wn = 0;
        break;
    default:
        break;
    }
    return 0;
}

static int hid_send(I2CSlave *i2c, uint8_t data)
{
    I2cHidDev *s = I2C_HID(i2c);

    if (s->wn < sizeof(s->wbuf)) {
        s->wbuf[s->wn++] = data;
    }
    return 0;
}

static uint8_t hid_recv(I2CSlave *i2c)
{
    I2cHidDev *s = I2C_HID(i2c);

    if (s->src && s->src_pos < s->src_len) {
        return s->src[s->src_pos++];
    }
    return 0;
}

static void hid_realize(DeviceState *dev, Error **errp)
{
    I2cHidDev *s = I2C_HID(dev);
    const char *k = s->kind_str ? s->kind_str : "mouse";
    const QemuInputHandler *h;

    if (!strcmp(k, "mouse")) {
        s->kind = K_MOUSE; s->rdesc = rd_mouse; s->rdesc_len = sizeof(rd_mouse); s->in_len = 7; h = &hid_rel_handler;
    } else if (!strcmp(k, "ptp")) {
        s->kind = K_PTP; s->rdesc = s->hybrid ? rd_ptp1 : rd_ptp; s->rdesc_len = s->hybrid ? sizeof(rd_ptp1) : sizeof(rd_ptp);
        s->in_len = s->hybrid ? 11 : 17; h = &hid_ptp_handler;
    } else if (!strcmp(k, "tpad")) {
        s->kind = K_TPAD; s->rdesc = rd_tpad; s->rdesc_len = sizeof(rd_tpad); s->in_len = 9; h = &hid_abs_handler;
    } else if (!strcmp(k, "touch")) {
        s->kind = K_TOUCH; s->rdesc = rd_touch; s->rdesc_len = sizeof(rd_touch); s->in_len = 8; h = &hid_abs_handler;
    } else if (!strcmp(k, "kbd")) {
        s->kind = K_KBD; s->rdesc = rd_kbd; s->rdesc_len = sizeof(rd_kbd); s->in_len = 11; h = &hid_kbd_handler;
    } else {
        error_setg(errp, "i2c-hid: unknown kind '%s'", k);
        return;
    }
    s->hs = qemu_input_handler_register(dev, h);
    qemu_input_handler_activate(s->hs);
    if (s->display) {
        qemu_input_handler_bind(s->hs, s->display, s->head, NULL);
    }
    if (s->gate_port) {
        memory_region_init_io(&s->gate_io, OBJECT(dev), &gate_ops, s, "i2c-hid-gate", 4);
        memory_region_add_subregion(get_system_io(), s->gate_port, &s->gate_io);
    }
}

static Property hid_props[] = {
    DEFINE_PROP_STRING("kind", I2cHidDev, kind_str),
    DEFINE_PROP_STRING("display", I2cHidDev, display),
    DEFINE_PROP_UINT32("head", I2cHidDev, head, 0),
    DEFINE_PROP_UINT16("desc-reg", I2cHidDev, desc_reg, REG_DESC_DEFAULT),
    DEFINE_PROP_BOOL("repeat", I2cHidDev, repeat, false),
    DEFINE_PROP_BOOL("hybrid", I2cHidDev, hybrid, false),
    DEFINE_PROP_UINT32("input-mode", I2cHidDev, input_mode, 0),
    DEFINE_PROP_UINT32("reports", I2cHidDev, reports, 0),
    DEFINE_PROP_UINT32("resets", I2cHidDev, resets, 0),
    DEFINE_PROP_UINT32("power", I2cHidDev, power, 1),
    DEFINE_PROP_UINT32("leds", I2cHidDev, leds, 0),
    DEFINE_PROP_UINT32("gate-port", I2cHidDev, gate_port, 0),
    DEFINE_PROP_BOOL("gate-open", I2cHidDev, gate_open, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void hid_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = hid_realize;
    dc->desc = "HID over I2C test device";
    k->event = hid_ev;
    k->recv = hid_recv;
    k->send = hid_send;
    device_class_set_props(dc, hid_props);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo hid_info = {
    .name = TYPE_I2C_HID,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(I2cHidDev),
    .class_init = hid_class_init,
};

static void hid_register(void)
{
    type_register_static(&hid_info);
}

type_init(hid_register)
