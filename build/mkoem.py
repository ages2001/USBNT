#!/usr/bin/env python3
"""Makes the USBNT driver disks for text-mode setup from the build output, one
per Windows family, each with every file in its root (TXTSETUP.OEM and
USBNT.TAG come from mkinf.py):

  out/oem/NT/     out/USBNTNT.IMG   Windows NT 3.1 - 4.0          USBNT.SYS (NT 3.x / 4.0 build)
  out/oem/2KXP/   out/USBNT2K.IMG   Windows 2000 / XP / 2003 x86  USBNT.SYS (i386), the three INFs
  out/oem/XP64/   out/USBNT64.IMG   Windows XP / 2003 x64         USBNT.SYS (amd64), the three INFs

A flat disk is what F6 / "S - Other" read, and what nLite and the other
integration tools expect: they take the files from the folder of
TXTSETUP.OEM / the INF and ignore the [Disks] directories.

  mkoem.py
"""
import os, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
OEM = os.path.join(OUT, 'oem')

README_NT = """USBNT driver disk - Windows NT 3.1, 3.5, 3.51, 4.0

At the screen that lists the mass storage devices press S, choose
"Other (Requires disk provided by a hardware manufacturer)", insert this
disk and choose "USBNT - Windows NT 3.1 to 4.0".

USBNT's disks (USB disks and sticks, card readers, NVMe drives behind
Thunderbolt / USB4) can then be installed to and installed from; the USB
keyboard works on.  See README.md of the release.
"""

README_2K = """USBNT driver disk - %s

F6 floppy: press F6 when "Press F6 if you need to install a third party
SCSI or RAID driver" appears, then S, insert this disk and choose:
  "USB 3 xHCI, USB4, card readers"  Windows keeps its own drivers for the
                                    USB 1.1 / 2.0 controllers
  "every USB controller"            USBNT drives every USB controller

nLite (and other tools that put text-mode drivers into the setup CD):
add this folder's USBNT.INF (or TXTSETUP.OEM) as a text-mode driver and
pick the same entry.  Every file the driver needs is in this folder; use
the folder of your Windows (2KXP for 32 bit Windows 2000 / XP / 2003,
XP64 for XP / 2003 x64).

USBNT's disks (USB disks and sticks, card readers, NVMe drives behind
Thunderbolt / USB4) can then be installed to and installed from; the USB
keyboard works on.  See README.md of the release.
"""

DISKS = [
    # folder, image, files {name on the disk: build output}, README
    ('NT', 'USBNTNT.IMG', {'USBNT.SYS': 'usbnt.sys'}, README_NT),
    ('2KXP', 'USBNT2K.IMG', {'USBNT.SYS': '2kxp/i386/usbnt.sys', 'USBNT.INF': '2kxp/usbnt.inf',
                             'USBNTKBD.INF': '2kxp/usbntkbd.inf', 'USBNTMOU.INF': '2kxp/usbntmou.inf'},
     README_2K % 'Windows 2000 / XP / Server 2003 (32 bit)'),
    ('XP64', 'USBNT64.IMG', {'USBNT.SYS': '2kxp/amd64/usbnt.sys', 'USBNT.INF': '2kxp/usbnt.inf',
                             'USBNTKBD.INF': '2kxp/usbntkbd.inf', 'USBNTMOU.INF': '2kxp/usbntmou.inf'},
     README_2K % 'Windows XP / Server 2003 x64'),
]


def main():
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    keep = set(d[0] for d in DISKS)
    for n in os.listdir(OEM):                       # what an older layout left behind
        p = os.path.join(OEM, n)
        if n not in keep:
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    for folder, img, files, readme in DISKS:
        d = os.path.join(OEM, folder)
        for f in ('TXTSETUP.OEM', 'USBNT.TAG'):
            if not os.path.exists(os.path.join(d, f)):
                sys.exit('%s/%s missing: run mkinf.py (wdmbuild.sh) first' % (folder, f))
        for name, src in files.items():
            if name.endswith('.INF'):
                # the INFs of the release name usbnt.sys in \\i386 / \\amd64 below them (2kxp\\);
                # here it is next to them - else GUI-mode setup (and nLite's PnP part) asks for it
                text = open(os.path.join(OUT, src), 'rb').read().decode('latin1')
                text = text.replace(',,,\\i386\r\n', ',,,\r\n').replace(',,,\\amd64\r\n', ',,,\r\n')
                open(os.path.join(d, name), 'wb').write(text.encode('latin1'))
            else:
                shutil.copyfile(os.path.join(OUT, src), os.path.join(d, name))
        open(os.path.join(d, 'README.TXT'), 'w', newline='\r\n').write(readme)
        names = ['TXTSETUP.OEM', 'USBNT.TAG', 'README.TXT'] + list(files)
        path = os.path.join(OUT, img)
        if os.path.exists(path):
            os.remove(path)
        subprocess.check_call(['mformat', '-C', '-f', '1440', '-v', 'USBNT', '-i', path, '::'], env=env)
        for name in names:
            subprocess.check_call(['mcopy', '-o', '-i', path, os.path.join(d, name), '::/' + name], env=env)
        used = sum(os.path.getsize(os.path.join(d, n)) for n in names)
        print('%s: %d bytes of files on a 1.44 MB disk' % (path, used))
        if used > 1400 * 1024:
            sys.exit('the files do not fit on the floppy')
    old = os.path.join(OUT, 'USBNTOEM.IMG')
    if os.path.exists(old):
        os.remove(old)


if __name__ == '__main__':
    main()
