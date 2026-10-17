#!/usr/bin/env python3
r"""Checks the USBNT driver disks (build/out/USBNTNT.IMG, USBNT2K.IMG, USBNT64.IMG,
or the images given) against the TXTSETUP.OEM format of every text-mode setup
they are meant for.

  toemfmt.py [IMG ...]

Each disk is for one Windows family and has every file in its root (the
[Disks] directory is \): nLite and the other integration tools take the files
from the folder of TXTSETUP.OEM and ignore the [Disks] directories - a disk
with \I386\USBNT.SYS made nLite leave usbnt.sys out of the CD ("The file
usbnt.sys could not be found" in Windows 2000 setup).

The rules come from Microsoft's TXTSETUP.OEM documentation (DDK: [Disks],
[Defaults], [HwComponent], [Files.HwComponent.ID], [Config.DriverKey],
[HardwareIds.scsi.ID]) and from the NT 3.x / 4.0 DDK form of the file:

  all versions  sections [Disks], [Defaults], [scsi]; "diskN = "description",
                \\tagfile, \\directory"; one [Files.scsi.ID] per [scsi] entry;
                "filetype = diskN, filename[, DriverKey]", DriverKey required
                for driver files of scsi components; 8.3 file names;
                "value = subkey, name, type, data" in [Config.DriverKey]
                (REG_DWORD: at most 8 hex digits); ';' comments; ASCII
  NT 3.1 - 4.0  file types driver, port, class, dll, inf, hal, detect (no
                "catalog"); [HardwareIds.*] is not read (ignored)
  2000 - 2003   file types driver, dll, inf, catalog, hal, detect;
                [HardwareIds.scsi.ID]: id = "device id", "service", the service
                being the driver file's name without .sys

and from what the disk must hold: the tag file, every listed file in its
disk's directory, for each family a choice whose driver is the right binary
(NT 3.1 - 4.0: i386 driver for subsystem 3.10; 2000 / XP: i386, 5.0;
x64: AMD64), INF files with a [Version] section naming the 2000 / XP ids.
"""
import os, re, struct, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
IMGS = sys.argv[1:] or [os.path.join(HERE, '..', 'build', 'out', n) for n in ('USBNTNT.IMG', 'USBNT2K.IMG', 'USBNT64.IMG')]
os.environ['MTOOLS_SKIP_CHECK'] = '1'

FAMILIES = {
    'NT 3.1': 'nt', 'NT 3.5': 'nt', 'NT 3.51': 'nt', 'NT 4.0': 'nt',
    'Windows 2000': '2k', 'Windows XP': '2k', 'Windows Server 2003': '2k',
    'Windows XP x64': 'x64', 'Windows Server 2003 x64': 'x64',
}
TYPES = {'nt': {'driver', 'port', 'class', 'dll', 'inf', 'hal', 'detect'},
         '2k': {'driver', 'dll', 'inf', 'catalog', 'hal', 'detect'},
         'x64': {'driver', 'dll', 'inf', 'catalog', 'hal', 'detect'}}
REG_TYPES = {'REG_DWORD', 'REG_SZ', 'REG_EXPAND_SZ', 'REG_BINARY', 'REG_MULTI_SZ'}
errors = []


def err(msg):
    errors.append(msg)


def fields(text):
    """the comma separated values of a line: quoted strings keep commas and blanks"""
    out, cur, q, quoted = [], '', False, False
    for ch in text:
        if ch == '"':
            q = not q
            quoted = True
        elif ch == ',' and not q:
            out.append((cur.strip(), quoted))
            cur, quoted = '', False
        else:
            cur += ch
    if q:
        raise ValueError('unbalanced quotes')
    out.append((cur.strip(), quoted))
    return out


def parse(raw):
    if b'\r\n' not in raw or raw.replace(b'\r\n', b'').find(b'\n') >= 0:
        err('TXTSETUP.OEM: lines must end in CR LF')
    try:
        text = raw.decode('ascii')
    except UnicodeDecodeError:
        err('TXTSETUP.OEM: not plain ASCII')
        text = raw.decode('latin1')
    sections, order, cur = {}, [], None
    for n, line in enumerate(text.split('\r\n'), 1):
        s = line.strip()
        if not s or s[0] in ';#':
            continue
        if s.startswith('['):
            if not s.endswith(']'):
                err('line %d: bad section header %r' % (n, s))
                continue
            cur = s[1:-1].strip().lower()
            if cur in sections:
                err('line %d: section [%s] twice' % (n, cur))
            sections[cur] = []
            order.append(cur)
            continue
        if cur is None:
            err('line %d: text before the first section' % n)
            continue
        if len(line) > 250:
            err('line %d: longer than 250 characters' % n)
        if '=' not in s:
            err('line %d: no "key = value": %r' % (n, s))
            continue
        k, v = s.split('=', 1)
        try:
            sections[cur].append((k.strip(), fields(v), n))
        except ValueError as e:
            err('line %d: %s' % (n, e))
    return sections


def is83(name):
    return re.match(r'^[A-Za-z0-9_$~!#%&\-{}()@^\'`]{1,8}(\.[A-Za-z0-9_$~!#%&\-{}()@^\'`]{0,3})?$', name) is not None


def pe_info(data):
    if data[:2] != b'MZ':
        return None
    e = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e:e + 4] != b'PE\0\0':
        return None
    machine = struct.unpack_from('<H', data, e + 4)[0]
    opt = e + 24
    sub_major, sub_minor = struct.unpack_from('<HH', data, opt + 48)
    subsystem = struct.unpack_from('<H', data, opt + 68)[0]
    return machine, (sub_major, sub_minor), subsystem


def check(IMG, covered):
    if not os.path.exists(IMG):
        sys.exit('no %s: run build/mkoem.py' % IMG)
    print('==', os.path.basename(IMG))
    tmp = tempfile.mkdtemp(prefix='toemfmt')
    if os.path.getsize(IMG) != 1474560:
        err('%s is not a 1.44 MB floppy image' % IMG)
    subprocess.check_call(['mcopy', '-s', '-n', '-i', IMG, '::/', tmp], stdout=subprocess.DEVNULL)
    files = {}
    for root, _, names in os.walk(tmp):
        for nm in names:
            p = os.path.join(root, nm)
            files['\\' + os.path.relpath(p, tmp).replace('/', '\\').upper()] = open(p, 'rb').read()
    if '\\TXTSETUP.OEM' not in files:
        sys.exit('no TXTSETUP.OEM in the root of the disk')
    sec = parse(files['\\TXTSETUP.OEM'])

    for need in ('disks', 'defaults', 'scsi'):
        if need not in sec:
            err('section [%s] missing' % need)
    disks = {}
    for k, v, n in sec.get('disks', []):
        if len(v) != 3:
            err('[Disks] line %d: want "description", \\tagfile, \\directory' % n)
            continue
        (desc, dq), (tag, _), (dirn, _) = v
        if not dq or not desc:
            err('[Disks] line %d: the description must be a quoted string' % n)
        if not tag.startswith('\\') or not dirn.startswith('\\'):
            err('[Disks] line %d: tag file and directory are paths from the root (\\...)' % n)
        if ':' in tag or ':' in dirn:
            err('[Disks] line %d: no drive letters' % n)
        if tag.upper() not in files:
            err('[Disks] line %d: tag file %s is not on the disk' % (n, tag))
        disks[k.lower()] = dirn.rstrip('\\')
        if dirn != '\\':
            err('[Disks] line %d: directory %s - every file must be in the root of the disk (nLite and the '
                'like ignore the directory and take the files from the folder of TXTSETUP.OEM)' % (n, dirn))
    options = {}
    for k, v, n in sec.get('scsi', []):
        if not v[0][1] or not v[0][0]:
            err('[scsi] line %d: the description must be a quoted string' % n)
        if len(v[0][0]) > 60:
            err('[scsi] line %d: description longer than 60 characters (setup menus cut it)' % n)
        if k.lower() in options:
            err('[scsi] line %d: id %s twice' % (n, k))
        options[k.lower()] = v[0][0]
    for k, v, n in sec.get('defaults', []):
        if k.lower() not in ('scsi', 'computer'):
            err('[Defaults] line %d: component %s' % (n, k))
        if k.lower() == 'scsi' and v[0][0].lower() not in options:
            err('[Defaults] line %d: %s is no [scsi] id' % (n, v[0][0]))

    keys = set()
    drivers = {}                                # option -> (driver file path, DriverKey, file types)
    for oid in options:
        s = sec.get('files.scsi.' + oid)
        if s is None:
            err('[Files.scsi.%s] missing' % oid)
            continue
        drv = None
        ftypes = set()
        for k, v, n in s:
            t = k.lower()
            ftypes.add(t)
            if len(v) < 2:
                err('[Files.scsi.%s] line %d: want diskN, filename[, DriverKey]' % (oid, n))
                continue
            disk, fname = v[0][0].lower(), v[1][0]
            if disk not in disks:
                err('[Files.scsi.%s] line %d: disk %s is not in [Disks]' % (oid, n, disk))
                continue
            if not is83(fname):
                err('[Files.scsi.%s] line %d: %s is not an 8.3 name' % (oid, n, fname))
            path = (disks[disk] + '\\' + fname).upper()
            if path not in files:
                err('[Files.scsi.%s] line %d: %s is not on the disk' % (oid, n, path))
            if t == 'driver':
                if len(v) < 3 or not v[2][0]:
                    err('[Files.scsi.%s] line %d: a scsi driver needs its DriverKey' % (oid, n))
                    continue
                if drv is not None:
                    err('[Files.scsi.%s]: two driver lines' % oid)
                drv = (path, v[2][0])
                keys.add(v[2][0].lower())
            if t == 'inf' and path in files:
                inf = files[path].decode('latin1')
                if '[Version]' not in inf or 'Signature="$Windows NT$"' not in inf:
                    err('%s: no [Version] Signature="$Windows NT$"' % path)
                sect = ''
                for line in inf.splitlines():
                    line = line.split(';')[0].strip()
                    if line.startswith('['):
                        sect = line.lower()
                    elif sect.startswith('[sourcedisksnames') and '=' in line:
                        f4 = (line.split('=', 1)[1].split(',') + ['', '', '', ''])[3].strip()
                        if f4 not in ('', '\\', '.'):
                            err('%s: %s puts the files in %s, not next to the INF' % (path, sect, f4))
        if drv is None:
            err('[Files.scsi.%s]: no driver line' % oid)
        else:
            drivers[oid] = (drv[0], drv[1], ftypes)
    for key in keys:
        s = sec.get('config.' + key)
        for k, v, n in s or []:
            if k.lower() != 'value' or len(v) < 4:
                err('[Config.%s] line %d: want value = subkey, name, type, data' % (key, n))
                continue
            typ, data = v[2][0].upper(), v[3][0]
            if typ not in REG_TYPES:
                err('[Config.%s] line %d: type %s' % (key, n, typ))
            if typ == 'REG_DWORD' and (len(v) != 4 or not re.match(r'^[0-9A-Fa-f]{1,8}$', data)):
                err('[Config.%s] line %d: REG_DWORD wants up to 8 hex digits' % (key, n))

    # which choice is for which Windows, and does it load there
    fam_of = {}
    for oid, (path, key, ftypes) in drivers.items():
        if path not in files:
            continue
        pe = pe_info(files[path])
        if pe is None:
            err('%s: not a PE image' % path)
            continue
        machine, subv, subsystem = pe
        if subsystem != 1:
            err('%s: not a native (driver) image' % path)
        if machine == 0x8664:
            fam_of[oid] = 'x64'
        elif machine == 0x14C and subv < (4, 0):
            fam_of[oid] = 'nt'
        elif machine == 0x14C and subv >= (5, 0):
            fam_of[oid] = '2k'
        else:
            err('%s: machine %x subsystem %d.%d fits no setup' % (path, machine, subv[0], subv[1]))
    for oid, fam in fam_of.items():
        bad = drivers[oid][2] - TYPES[fam]
        if bad:
            err('[Files.scsi.%s]: file type(s) %s unknown to %s setup' % (oid, ', '.join(sorted(bad)), fam))
        hw = sec.get('hardwareids.scsi.' + oid)
        service = os.path.splitext(drivers[oid][0].split('\\')[-1])[0].lower()
        if fam == 'nt':
            if hw:
                print('note: [HardwareIds.scsi.%s] is not read by NT 3.x / 4.0 setup' % oid)
            continue
        if not hw:
            err('[HardwareIds.scsi.%s] missing: a 2000 / XP Plug and Play driver needs its ids' % oid)
            continue
        ids = set()
        for k, v, n in hw:
            if k.lower() != 'id' or len(v) < 2 or not v[0][1] or not v[1][1]:
                err('[HardwareIds.scsi.%s] line %d: want id = "device id", "service"' % (oid, n))
                continue
            dev, svc = v[0][0], v[1][0]
            if svc.lower() != service:
                err('[HardwareIds.scsi.%s] line %d: service %s is not the driver %s' % (oid, n, svc, service))
            if not re.match(r'^(PCI\\(CC_[0-9A-F]{4,6}|VEN_[0-9A-F]{4}(&DEV_[0-9A-F]{4})?(&SUBSYS_[0-9A-F]{8})?'
                            r'(&REV_[0-9A-F]{2})?(&CC_[0-9A-F]{4,6})?)|'
                            r'USBNT\\[A-Za-z]+|ACPI\\[0-9A-Z]{7,8})$', dev, re.I):
                err('[HardwareIds.scsi.%s] line %d: odd device id %s' % (oid, n, dev))
            if dev.upper() in ids:
                err('[HardwareIds.scsi.%s] line %d: %s twice' % (oid, n, dev))
            ids.add(dev.upper())
            if len(v) > 2 and not re.match(r'^\{[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}\}$', v[2][0], re.I):
                err('[HardwareIds.scsi.%s] line %d: third value is no class GUID' % (oid, n))
        if 'PCI\\CC_0C0330' not in ids:
            err('[HardwareIds.scsi.%s]: xHCI (PCI\\CC_0C0330) missing' % oid)
        # the INF must offer the same PCI ids, so GUI-mode setup finds a driver for these devices
        inf = files.get('\\USBNT.INF', b'').decode('latin1').upper()
        for d in ids:
            if d.startswith('PCI\\') and (', ' + d) not in inf:
                err('%s (HardwareIds.scsi.%s) is not in USBNT.INF' % (d, oid))
    fams = set(fam_of.values())
    if len(fams) > 1:
        err('choices for more than one Windows family (%s) on one disk: an integration tool would take '
            'one usbnt.sys for all' % ', '.join(sorted(fams)))
    for os_name, fam in FAMILIES.items():
        mine = [o for o, f in fam_of.items() if f == fam]
        if mine:
            covered.add(fam)
            print('%-24s %d choice(s): %s' % (os_name, len(mine), '; '.join(options[o] for o in mine)))


def main():
    covered = set()
    for img in IMGS:
        check(img, covered)
    for os_name, fam in FAMILIES.items():
        if fam not in covered:
            err('no choice for %s on any disk' % os_name)
    for e in errors:
        print('ERROR', e)
    print('RESULT txtsetup.oem format', 'PASS' if not errors else 'FAIL (%d)' % len(errors))
    return 0 if not errors else 1


if __name__ == '__main__':
    sys.exit(main())
