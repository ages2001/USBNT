/* pnpeject.c: Windows 2000 / XP eject of a USBNT disk slot through Plug and Play.
   Returns 1 ejected, 0 Windows refused (Veto says why), -1 not a Plug and Play
   system or no such disk (then ask usbnt.sys with IOCTL_USBNT_EJECT). */
int PnpEjectSlot(DWORD Slot, char *Veto, int VetoSize);
