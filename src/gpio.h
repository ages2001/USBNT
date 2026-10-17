/*
 * gpio.h - GPIO pads of Intel PCHs / SoCs, Bay Trail / Cherry Trail and AMD
 * FCH, for the HID over I2C code (i2chid.c): the power / reset GpioIo lines of
 * a touch pad and its GpioInt "data ready" line, and the GeneralPurposeIO
 * operation regions of _PS0 (through aml.c). Pads are read (input level) and
 * driven (output); no interrupt is taken (NT polls the pad level cheaply).
 *
 * Ported from AUDIONT (drv/gpio.c, gpiotab.h), extended for i2chid.
 */
#ifndef GPIO_H
#define GPIO_H

typedef struct _GPIO_PIN {
    void   *Cfg;                /* pad config register VA (NULL: not resolved) */
    ULONG   Pad;
    char    Ctrl[5];
    USHORT  Pin;                /* ACPI pin number */
    UCHAR   Kind;
} GPIO_PIN;

/* resolve an ACPI pin on the controller named by its last name segment
   ("GPO0", "GPO1", ...); FALSE when the controller or pin is unknown */
BOOLEAN GpioResolvePin(const char *Ctrl4, ULONG Pin, GPIO_PIN *P);
void    GpioSet(GPIO_PIN *P, BOOLEAN High);     /* drive the pad as a GPIO output */
int     GpioGet(GPIO_PIN *P);                   /* input level 0/1, -1 unknown */
void    GpioCloseAll(void);

#endif
