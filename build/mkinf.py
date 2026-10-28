#!/usr/bin/env python3
r"""Writes the Windows 2000 / XP INF files of USBNT (out/2kxp/*.inf) from the
id lists in the sources: USB host controllers, card readers, Thunderbolt /
USB4 host interfaces, Intel LPSS I2C controllers and the ACPI I2C
controllers, the USBNT device bus, keyboard and mouse.

UHCI, OHCI and EHCI controllers are named only by PCI\CC_0C03, the last and
weakest of the compatible ids Windows' PCI driver gives them: every INF of
Windows that has a driver for a controller (usb.inf, usbport.inf, usb2.inf:
PCI\CC_0C0300 / 0C0310 / 0C0320 and vendor ids) ranks better, so when Windows
looks for a driver itself ("Found New Hardware") it takes its own, and USBNT
only where Windows has none (EHCI on Windows 2000 before SP4 and XP before
SP1).  usbinst installs USBNT on a controller explicitly when its policy says
so.  The date is older than any of Windows' USB INFs for the same reason.
xHCI and USB4 (which Windows 2000 / XP do not support) have their class codes.

Also writes out/oem/<NT, 2KXP, XP64>/TXTSETUP.OEM, the files text-mode setup reads from the
USBNT driver disks (mkoem.py makes them; one per family, every file in the root): one entry for NT 3.1 - 4.0,
two each for Windows 2000 / XP (x86) and XP / Server 2003 x64 - the USB 3,
USB4 and card reader controllers only (Windows keeps its own USB 1.1 / 2.0
drivers and their keyboard support in setup) or every USB controller.
"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
OUT = os.path.join(HERE, 'out', '2kxp')
VER = '06/01/1999,1.2.0.0'
os.makedirs(OUT, exist_ok=True)

lpss = re.findall(r'\{ 0x([0-9a-f]{4}),', open(os.path.join(SRC, 'lpssids.h')).read())
rtsx = re.findall(r'\{ 0x([0-9A-F]{4}), "R', open(os.path.join(SRC, 'rtsxpci.c')).read())
acpi_i2c = [n for n in re.findall(r'\{ "([0-9A-Z]{7,8})", \d+, \d+ \}', open(os.path.join(SRC, 'i2chid.c')).read())
            if n != 'AMDI0019']
tbt_src = open(os.path.join(SRC, 'tbt.c')).read()
gen = tbt_src[tbt_src.index('static UCHAR Generation(USHORT Id)'):]
gen = gen[:gen.index('default:')]
tbt = re.findall(r'case 0x([0-9A-F]{4}):', gen)
# host interfaces that share their device id with their own PCIe bridges: named with the class (0880)
shared = re.findall(r'0x([0-9A-F]{4})', tbt_src[tbt_src.index('SharedIds[] = {'):].split('}')[0])
# the names of the controllers (ChipName in tbt.c)
chip = {}
cn = tbt_src[tbt_src.index('static const char *ChipName('):]
cn = cn[:cn.index('default:')]
for ids, name in re.findall(r'((?:case 0x[0-9A-F]{4}: )+)return "([^"]+)";', cn):
    for d in re.findall(r'0x([0-9A-F]{4})', ids):
        chip[d] = name


models = [('xhci', 'PCI\\CC_0C0330'), ('usb4', 'PCI\\CC_0C0340'), ('usbhc', 'PCI\\CC_0C03')]
for d in tbt:
    models.append(('tbt_' + d, 'PCI\\VEN_8086&DEV_%s%s' % (d, '&CC_0880' if d in shared else '')))
models.append(('sdhci', 'PCI\\CC_080501'))
models.append(('sdhci', 'PCI\\CC_080500'))
for d in rtsx:
    models.append(('rtsx', 'PCI\\VEN_10EC&DEV_%s' % d))
for d in lpss:
    models.append(('i2c', 'PCI\\VEN_8086&DEV_%s' % d.upper()))
for a in acpi_i2c:
    models.append(('i2c', 'ACPI\\%s' % a))
# Device Manager: USBNT's root hubs, hubs and devices (wdmtree.c; the names come from the driver)
models.append(('hub', 'USBNT\\HUB'))
models.append(('node', 'USBNT\\DEVICE'))
models.append(('bus', 'USBNT\\Bus'))

DESC = {
    'xhci': 'USBNT USB 3.x eXtensible Host Controller (xHCI)',
    'usbhc': 'USBNT USB 1.1 / 2.0 Host Controller (UHCI, OHCI, EHCI)',
    'usb4': 'USBNT USB4 / Thunderbolt 4 / Thunderbolt 5 Host Router',
    'tbt': 'USBNT Thunderbolt Host Router',
    'sdhci': 'USBNT SD Card Reader (SDHCI)',
    'rtsx': 'USBNT SD Card Reader (Realtek PCIe)',
    'i2c': 'USBNT HID over I2C Controller (touch pad, touch screen)',
    'bus': 'USBNT Device Bus (USB keyboards, mice, disks, UAS)',
    'hub': 'USBNT USB Hub',
    'node': 'USBNT USB Device',
}


def wr(name, text):
    # INF files: plain ASCII with CRLF (Windows 2000 reads them as ANSI)
    open(os.path.join(OUT, name), 'w', newline='\r\n').write(text)


def common(cls, guid):
    return ('[Version]\nSignature="$Windows NT$"\nClass=%s\nClassGUID=%s\nProvider=%%USBNT%%\nDriverVer=%s\n\n'
            '[SourceDisksNames.x86]\n1 = %%Disk%%,,,\\i386\n\n[SourceDisksNames.amd64]\n1 = %%Disk%%,,,\\amd64\n\n'
            '[SourceDisksFiles]\nusbnt.sys = 1\n\n[DestinationDirs]\nDefaultDestDir = 12\n\n') % (cls, guid, VER)


# boot start (0), like text-mode setup installs it: Windows can then start from a USB disk
SERVICE = ('[USBNT_Service]\nDisplayName    = %SvcDesc%\nServiceType    = 1\nStartType      = 0\nErrorControl   = 1\n'
           'ServiceBinary  = %12%\\usbnt.sys\nLoadOrderGroup = SCSI miniport\nAddReg         = USBNT_Params\n\n'
           '[USBNT_Params]\nHKR,Parameters,DebugLevel,0x00010003,2\n\n[USBNT_Files]\nusbnt.sys\n\n')

# usbnt.inf: the PCI (and ACPI) functions and the device bus
seen = set()
lines = []
linesxp = []
for k, i in models:
    if i.upper() in seen:
        continue
    seen.add(i.upper())
    sect = 'USBNT_Bus' if k in ('bus', 'hub', 'node') else 'USBNT_HC'
    lines.append('%%%s.Desc%% = %s, %s' % (k, sect, i))
    linesxp.append('%%%s.Desc%% = %sX, %s' % (k, sect, i))
body = common('USB', '{36FC9E60-C465-11CF-8056-444553540000}')
# Windows 2000 reads [USBNT], XP (x86) [USBNT.NTx86.5.1], XP x64 [USBNT.NTamd64]
body += '[Manufacturer]\n%USBNT% = USBNT, NTx86.5.1, NTamd64\n\n'
body += ('[USBNT]\n' + '\n'.join(lines) + '\n\n[USBNT.NTx86.5.1]\n' + '\n'.join(linesxp) +
         '\n\n[USBNT.NTamd64]\n' + '\n'.join(linesxp) + '\n\n')
# usbd.sys (the USB library Windows' USB class drivers import) comes from Windows' own USB INF:
# without Windows' USB stack nothing else would install it (XP: usbport.inf)
# (Windows 2000's own USB class INFs bring it themselves)
for sect, extra in (('USBNT_HC', ''), ('USBNT_Bus', ''),
                    ('USBNT_HCX', 'Include = usbport.inf\n'), ('USBNT_BusX', 'Include = usbport.inf\n')):
    body += ('[%s.NT]\n%sCopyFiles = USBNT_Files%s\n\n'
             '[%s.NT.Services]\nAddService = usbnt, 0x00000002, USBNT_Service\n\n'
             % (sect, extra, ', HUB20.CopyFiles.NT' if extra else '', sect))
body += SERVICE
body += '[Strings]\nUSBNT = "USBNT"\nDisk = "USBNT disk"\nSvcDesc = "USBNT USB driver"\n'
for k, d in DESC.items():
    body += '%s.Desc = "%s"\n' % (k, d)
for d in tbt:
    body += 'tbt_%s.Desc = "USBNT %s Host Router"\n' % (d, chip.get(d, 'Thunderbolt'))
wr('usbnt.inf', body)

for name, cls, guid, hwid, desc, extra in (
        ('usbntkbd.inf', 'Keyboard', '{4D36E96B-E325-11CE-BFC1-08002BE10318}', 'USBNT\\Keyboard',
         'USBNT USB Keyboard', 'kbdclass'),
        ('usbntmou.inf', 'Mouse', '{4D36E96F-E325-11CE-BFC1-08002BE10318}', 'USBNT\\Mouse',
         'USBNT USB Mouse', 'mouclass')):
    body = common(cls, guid)
    body += '[Manufacturer]\n%USBNT% = USBNT, NTamd64\n\n'
    body += '[USBNT]\n%%Dev.Desc%% = USBNT_Dev, %s\n\n[USBNT.NTamd64]\n%%Dev.Desc%% = USBNT_Dev, %s\n\n' % (hwid, hwid)
    # kbdclass / mouclass come with the class (its UpperFilters)
    body += ('[USBNT_Dev.NT]\nCopyFiles = USBNT_Files\n\n[USBNT_Dev.NT.Services]\n'
             'AddService = usbnt, 0x00000002, USBNT_Service\n\n')
    body += SERVICE
    body += '[Strings]\nUSBNT = "USBNT"\nDisk = "USBNT disk"\nSvcDesc = "USBNT USB driver"\nDev.Desc = "%s"\n' % desc
    wr(name, body)
print('INF files in', OUT, '-', len(lines), 'device ids')


# ---------------------------------------------------------------------------
# TXTSETUP.OEM (text-mode setup, NT 3.1 - Windows Server 2003)
# ---------------------------------------------------------------------------
OEM = os.path.join(HERE, 'out', 'oem')
os.makedirs(OEM, exist_ok=True)
KBD_GUID = '{4D36E96B-E325-11CE-BFC1-08002BE10318}'
MOU_GUID = '{4D36E96F-E325-11CE-BFC1-08002BE10318}'
new_ids = ['PCI\\CC_0C0330', 'PCI\\CC_0C0340'] + \
          ['PCI\\VEN_8086&DEV_%s%s' % (d, '&CC_0880' if d in shared else '') for d in tbt] + \
          ['PCI\\CC_080501', 'PCI\\CC_080500'] + ['PCI\\VEN_10EC&DEV_%s' % d for d in rtsx]
# (UHCI / OHCI / EHCI: by the class code id usbnt.inf names them with - see above)
old_ids = ['PCI\\CC_0C03']
# One driver disk (folder) per Windows family, every file in the folder of TXTSETUP.OEM:
# nLite and the other integration tools take the files from the folder of TXTSETUP.OEM /
# the INF and ignore the [Disks] directories, so a disk with \I386\USBNT.SYS made them
# put usbnt.sys into TXTSETUP.SIF without copying it ("The file usbnt.sys could not be
# found" in text-mode setup).  A disk of its own per family also keeps them from taking
# the binary of another Windows.
FAMILIES = [
    # folder, disk title, options: (id, description (setup's menu), all USB controllers), with INFs
    ('NT', 'USBNT driver disk - Windows NT 3.1 to 4.0',
     [('usbntnt', 'USBNT - Windows NT 3.1 to 4.0', False)], False),
    ('2KXP', 'USBNT driver disk - Windows 2000 / XP / 2003 (x86)',
     [('usbnt2k', 'USBNT - Windows 2000 / XP: USB 3 xHCI, USB4, card readers', False),
      ('usbnt2ka', 'USBNT - Windows 2000 / XP: every USB controller', True)], True),
    ('XP64', 'USBNT driver disk - Windows XP / 2003 x64',
     [('usbnt64', 'USBNT - Windows XP / 2003 x64: USB 3 xHCI, USB4, readers', False),
      ('usbnt64a', 'USBNT - Windows XP / 2003 x64: every USB controller', True)], True),
]
for folder, title, options, infs in FAMILIES:
    t = ['; TXTSETUP.OEM - %s (generated by build/mkinf.py)' % title,
         ';']
    if not infs:
        t += ['; Windows NT 3.1, 3.5, 3.51, 4.0: at the mass storage screen press S, choose',
              ';   "Other", insert this disk and choose "USBNT - Windows NT 3.1 to 4.0".']
    else:
        t += ['; Press F6 when setup starts, then S at "Setup could not determine the type of',
              ';   one or more mass storage devices", insert this disk and choose an entry.',
              ';   "USB 3 xHCI, USB4, card readers": Windows keeps its own drivers for',
              ';   USB 1.1 / 2.0 controllers; "every USB controller": USBNT drives them all.',
              '; nLite and similar tools: point them at USBNT.INF (or this file) in this folder.']
    t += [';',
          '; Every file is in the root of the disk (the folder of this file).',
          '',
          '[Disks]',
          'd1 = "%s", \\usbnt.tag, \\' % title,
          '',
          '[Defaults]',
          'scsi = %s' % options[0][0],
          '',
          '[scsi]']
    for oid, desc, allhc in options:
        t.append('%-8s = "%s", usbnt' % (oid, desc))
    for oid, desc, allhc in options:
        t += ['', '[Files.scsi.%s]' % oid, 'driver = d1, usbnt.sys, usbnt']
        if infs:
            t += ['inf    = d1, usbnt.inf', 'inf    = d1, usbntkbd.inf', 'inf    = d1, usbntmou.inf']
    for oid, desc, allhc in options:
        if not infs:
            continue
        t += ['', '[HardwareIds.scsi.%s]' % oid]
        seen_oem = set()
        for i in new_ids + (old_ids if allhc else []):
            if i.upper() not in seen_oem:
                seen_oem.add(i.upper())
                t.append('id = "%s", "usbnt"' % i)
        # the keyboard and mouse of the USBNT device bus, with their device classes (kbdclass, mouclass)
        t.append('id = "USBNT\\Keyboard", "usbnt", "%s"' % KBD_GUID)
        t.append('id = "USBNT\\Mouse", "usbnt", "%s"' % MOU_GUID)
    t += ['',
          '; usbnt.sys reads its parameters from Services\\usbnt\\Parameters (README.md);',
          '; OemSetup tells usbinst that text-mode setup installed the driver',
          '[Config.usbnt]',
          'value = Parameters, OemSetup, REG_DWORD, 1',
          '']
    d = os.path.join(OEM, folder)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'TXTSETUP.OEM'), 'w', newline='\r\n').write('\n'.join(t))
    open(os.path.join(d, 'USBNT.TAG'), 'w', newline='\r\n').write('%s\n' % title)
for old in ('TXTSETUP.OEM', 'USBNT.TAG'):
    if os.path.exists(os.path.join(OEM, old)):
        os.remove(os.path.join(OEM, old))
print('TXTSETUP.OEM in', OEM, '-', ', '.join(f[0] for f in FAMILIES))
