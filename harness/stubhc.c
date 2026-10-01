/*
 * stubhc.c - placeholder host controller tables for harness builds that
 * leave some controller types out (see HCS in build.sh).
 */

#include "../src/usbnt.h"

#if !defined(HAVE_UHCI) || !defined(HAVE_OHCI) || !defined(HAVE_EHCI)
static int NoStart(USB_HC *h)
{
    (void)h;
    return USB_ERR_NODEV;
}
#endif

#ifndef HAVE_UHCI
const HCD_OPS UhciOps = { "UHCI", NoStart, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
#endif
#ifndef HAVE_OHCI
const HCD_OPS OhciOps = { "OHCI", NoStart, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
#endif
#ifndef HAVE_EHCI
const HCD_OPS EhciOps = { "EHCI", NoStart, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
#endif
