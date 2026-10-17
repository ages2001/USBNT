/*
 * usbvideo.h - USB Video Class cameras (usbvideo.c)
 */
#ifndef USBVIDEO_H
#define USBVIDEO_H

#include "usbnt.h"
#include "usbntioc.h"

int     UvcAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End);
void    UvcDetach(USB_DEV *Dev);
void    UvcService(void);                   /* worker */
void    UvcList(USBNT_CAMERAS *Out);
int     UvcStart(const USBNT_VID_START *In, USBNT_VID_STATUS *Out);
int     UvcStop(ULONG Id);
int     UvcControl(USBNT_VID_CONTROL *Ctl);
LONG    UvcGetFrame(const USBNT_VID_GET *Get, UCHAR *Out, ULONG Size);

#endif
