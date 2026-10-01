"""Helpers to run a Windows NT guest in QEMU and drive it over QMP."""
import json
import os
import socket
import subprocess
import tempfile
import time

KEYMAP = {
    " ": "spc", "\n": "ret", "\t": "tab", "\\": "backslash", "/": "slash", ".": "dot",
    ",": "comma", "-": "minus", "=": "equal", ";": "semicolon", "'": "apostrophe",
    "[": "bracket_left", "]": "bracket_right", "`": "grave_accent",
    ":": "shift-semicolon", "_": "shift-minus", "\"": "shift-apostrophe", "*": "shift-8",
    ">": "shift-dot", "<": "shift-comma", "?": "shift-slash", "|": "shift-backslash",
    "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4", "%": "shift-5",
    "&": "shift-7", "(": "shift-9", ")": "shift-0", "+": "shift-equal",
}


class NtVm:
    def __init__(self, args, com2=None, mem=128, machine=None, cpu="pentium3"):
        machine = machine or os.environ.get("NTVM_MACHINE", "q35")
        self.dir = tempfile.mkdtemp(prefix="ntvm")
        self.qmp_path = os.path.join(self.dir, "qmp")
        self.com2 = com2 or os.path.join(self.dir, "com2.log")
        cmd = ["qemu-system-i386", "-machine", machine, "-cpu", cpu, "-m", str(mem),
               "-display", "none", "-vga", "none", "-device", "cirrus-vga,id=vga", "-rtc", "base=localtime",
               "-qmp", "unix:%s,server=on,wait=off" % self.qmp_path,
               "-serial", "null", "-serial", "file:%s" % self.com2] + args
        if os.environ.get("NTVM_EXTRA"):
            cmd += os.environ["NTVM_EXTRA"].split()
        if os.environ.get("NTVM_SOCK"):
            cmd[cmd.index("file:%s" % self.com2)] = "unix:%s,server=on,wait=off" % os.environ["NTVM_SOCK"]
        self.qlog = os.environ.get("NTVM_QLOG") or os.path.join(self.dir, "qemu.out")
        self.proc = subprocess.Popen(cmd, stdout=open(self.qlog, "wb"), stderr=subprocess.STDOUT)
        for _ in range(200):
            try:
                self.qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.qmp.connect(self.qmp_path)
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError(open(self.qlog, errors="replace").read())
                time.sleep(0.05)
        self.qbuf = b""
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            while b"\n" in self.qbuf:
                line, self.qbuf = self.qbuf.split(b"\n", 1)
                msg = json.loads(line)
                if "event" in msg:
                    continue
                return msg
            self.qbuf += self.qmp.recv(65536)

    def cmd(self, name, **args):
        self.qmp.sendall((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
        r = self._read()
        if "error" in r:
            raise RuntimeError("%s: %s" % (name, r["error"]))
        return r.get("return")

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

    def shot(self, path):
        ppm = path + ".ppm"
        self.cmd("screendump", filename=ppm)
        time.sleep(0.3)
        subprocess.call(["convert", ppm, path])
        os.unlink(ppm)
        return path

    def key(self, k, hold=None):
        if hold:
            self.hmp("sendkey %s %d" % (k, hold))
        else:
            self.hmp("sendkey %s" % k)
        time.sleep(0.12)

    def type(self, text):
        for ch in text:
            if ch in KEYMAP:
                k = KEYMAP[ch]
            elif ch.isupper():
                k = "shift-" + ch.lower()
            else:
                k = ch
            self.key(k)

    QCODE = {" ": "spc", "\n": "ret", "\\": "backslash", "/": "slash", ".": "dot", ",": "comma",
             "-": "minus", "=": "equal", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
             "]": "bracket_right", "`": "grave_accent", "\t": "tab"}
    SHIFTED = {":": "semicolon", "_": "minus", "\"": "apostrophe", "*": "8", ">": "dot", "<": "comma",
               "?": "slash", "|": "backslash", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
               "&": "7", "(": "9", ")": "0", "+": "equal", "{": "bracket_left", "}": "bracket_right"}

    def _keyev(self, code, down, device):
        ev = {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": code}}}
        args = {"events": [ev]}
        if device:
            args["device"] = device
        self.cmd("input-send-event", **args)

    def press(self, codes, device=None, hold=0.06):
        if isinstance(codes, str):
            codes = [codes]
        for c in codes:
            self._keyev(c, True, device)
        time.sleep(hold)
        for c in reversed(codes):
            self._keyev(c, False, device)
        time.sleep(0.06)

    def typ(self, text, device=None):
        for ch in text:
            if ch in self.QCODE:
                self.press(self.QCODE[ch], device)
            elif ch in self.SHIFTED:
                self.press(["shift", self.SHIFTED[ch]], device)
            elif ch.isupper():
                self.press(["shift", ch.lower()], device)
            else:
                self.press(ch, device)

    def mouse(self, dx=0, dy=0, wheel=0, buttons=None, device=None):
        evs = []
        if dx or dy:
            evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
            evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        if wheel:
            btn = "wheel-up" if wheel > 0 else "wheel-down"
            for _ in range(abs(wheel)):
                for down in (True, False):
                    a = {"events": [{"type": "btn", "data": {"down": down, "button": btn}}]}
                    if device:
                        a["device"] = device
                    self.cmd("input-send-event", **a)
                    time.sleep(0.05)
        if buttons:
            for b, down in buttons:
                evs.append({"type": "btn", "data": {"down": down, "button": b}})
        if evs:
            a = {"events": evs}
            if device:
                a["device"] = device
            self.cmd("input-send-event", **a)
        time.sleep(0.05)

    def abs(self, x, y, buttons=None, device=None):
        """Absolute pointer position, 0..32767 on both axes."""
        evs = [{"type": "abs", "data": {"axis": "x", "value": x}},
               {"type": "abs", "data": {"axis": "y", "value": y}}]
        for b, down in (buttons or []):
            evs.append({"type": "btn", "data": {"down": down, "button": b}})
        a = {"events": evs}
        if device:
            a["device"] = device
        self.cmd("input-send-event", **a)
        time.sleep(0.05)

    def log(self):
        try:
            return open(self.com2, "rb").read().decode("latin1")
        except OSError:
            return ""

    def wait_log(self, text, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(1)
        return False

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        try:
            self.proc.kill()
            self.proc.wait(5)
        except Exception:
            pass
