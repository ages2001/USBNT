"""Prepares a copy of an NT disk image with usbnt.sys installed offline."""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.environ.get("USBNT_SYS") or os.path.join(HERE, "..", "build", "out", "usbnt.sys")


def mtools_rc(image, offset=32256):
    rc = os.path.join(tempfile.gettempdir(), "mtoolsrc_%d" % os.getpid())
    with open(rc, "w") as f:
        f.write('drive n: file="%s" offset=%d\nmtools_skip_check=1\n' % (image, offset))
    os.environ["MTOOLSRC"] = rc


def hivexsh(hive, script, write=False):
    fd, path = tempfile.mkstemp(suffix=".hsh")
    with os.fdopen(fd, "w") as f:
        f.write(script)
    args = ["hivexsh"] + (["-w"] if write else []) + ["-f", path, hive]
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.unlink(path)
    return r


def hive_values(hive, key):
    script = "cd %s\nlsval\n" % key
    out = hivexsh(hive, script).stdout.decode()
    vals = []
    for line in out.splitlines():
        m = re.match(r'"([^"]*)"=(.*)$', line)
        if not m:
            continue
        name, v = m.group(1), m.group(2)
        if v.startswith("dword:"):
            vals.append((name, "dword:0x" + v[6:]))
        elif v.startswith('"'):
            vals.append((name, "string:" + re.sub(r"\\(.)", r"\1", v[1:-1])))
        elif v.startswith("hex("):
            m2 = re.match(r"hex\((\d+)\):(.*)$", v)
            vals.append((name, "hex:%s:%s" % (m2.group(1), m2.group(2))))
        elif v.startswith("str(2):"):
            vals.append((name, "expandstring:" + re.sub(r"\\(.)", r"\1", v[8:-1])))
        else:
            vals.append((name, None))
    return vals


def set_values(hive, key, changes, create=False):
    """changes: dict name -> hivexsh value spec. Keeps other values."""
    parent, leaf = key.rsplit("\\", 1)
    script = ""
    existing = []
    if create:
        script += "cd %s\nadd %s\n" % (parent, leaf)
    else:
        existing = hive_values(hive, key)
        if any(v is None for _, v in existing):
            raise RuntimeError("unsupported value type in %s" % key)
    vals = [(n, v) for n, v in existing if n not in changes] + list(changes.items())
    script += "cd %s\nsetval %d\n" % (key, len(vals))
    for n, v in vals:
        script += "%s\n%s\n" % (n, v)
    script += "commit\n"
    r = hivexsh(hive, script, write=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.decode())


def prepare(base, out, windir="WINNT351", params=None, extra_files=(), setup_cmd=None, no_ps2=False, start=None):
    shutil.copyfile(base, out)
    mtools_rc(out)
    subprocess.check_call(["mcopy", "-o", DRIVER, "n:/%s/system32/drivers/usbnt.sys" % windir])
    for src, dst in extra_files:
        subprocess.check_call(["mcopy", "-o", src, "n:/" + dst])
    tmp = tempfile.mkdtemp()
    hive = os.path.join(tmp, "system")
    subprocess.check_call(["mcopy", "-o", "n:/%s/system32/config/system" % windir, hive])
    svc = {
        "Type": "dword:0x1", "Start": "dword:0x0", "ErrorControl": "dword:0x1",
        "Group": "string:SCSI miniport", "ImagePath": "expandstring:system32\\drivers\\usbnt.sys",
    }
    for k, v in (params or {}).items():
        svc[k] = "dword:0x%x" % v
    done = 0
    for cs in ("ControlSet001", "ControlSet002"):
        exists = hivexsh(hive, "cd \\%s\\Services\n" % cs).returncode == 0
        if not exists:
            continue
        created = hivexsh(hive, "cd \\%s\\Services\\usbnt\n" % cs).returncode == 0
        set_values(hive, "\\%s\\Services\\usbnt" % cs, svc, create=not created)
        set_values(hive, "\\%s\\Services\\Kbdclass\\Parameters" % cs, {"ConnectMultiplePorts": "dword:0x1"})
        set_values(hive, "\\%s\\Services\\Mouclass\\Parameters" % cs, {"ConnectMultiplePorts": "dword:0x1"})
        for svcname, val in (start or {}).items():
            set_values(hive, "\\%s\\Services\\%s" % (cs, svcname), {"Start": "dword:0x%x" % val})
        if no_ps2:
            set_values(hive, "\\%s\\Services\\i8042prt" % cs, {"Start": "dword:0x4"})
        if hivexsh(hive, "cd \\%s\\Control\\CrashControl\n" % cs).returncode == 0:
            set_values(hive, "\\%s\\Control\\CrashControl" % cs, {"AutoReboot": "dword:0x0"})
        done += 1
    if done == 0:
        raise RuntimeError("no control set found")
    if setup_cmd is not None:
        set_values(hive, "\\Setup", {"CmdLine": "string:" + setup_cmd, "SetupType": "dword:0x1"})
    subprocess.check_call(["mcopy", "-o", hive, "n:/%s/system32/config/system" % windir])
    shutil.rmtree(tmp)
    return out


if __name__ == "__main__":
    prepare(sys.argv[1], sys.argv[2])
