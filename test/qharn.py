"""QEMU driver for the bare metal USB test kernel."""
import json
import os
import socket
import subprocess
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = os.path.join(HERE, "..", "harness", "kernel.elf")


class Harness:
    def __init__(self, args, kernel=KERNEL, log=None, mem=256):
        self.dir = tempfile.mkdtemp(prefix="usbq")
        self.ser_path = os.path.join(self.dir, "ser")
        self.qmp_path = os.path.join(self.dir, "qmp")
        self.log = open(log or os.path.join(self.dir, "serial.log"), "w")
        cmd = ["qemu-system-i386", "-machine", "pc", "-m", str(mem), "-display", "none",
               "-nodefaults", "-kernel", kernel,
               "-chardev", "socket,id=ser,path=%s,server=on,wait=on" % self.ser_path,
               "-serial", "chardev:ser",
               "-qmp", "unix:%s,server=on,wait=off" % self.qmp_path] + args
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.ser = self._connect(self.ser_path)
        self.ser.settimeout(0.05)
        self.buf = b""
        self.pending = []
        self.all = ""
        self.qmp = self._connect(self.qmp_path)
        self.qmp.settimeout(5)
        self.qbuf = b""
        self._qmp_read()
        self._qmp_cmd({"execute": "qmp_capabilities"})

    def _connect(self, path):
        for _ in range(200):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError("qemu exited: %s" % self.proc.stdout.read().decode())
                time.sleep(0.05)
        raise RuntimeError("cannot connect to %s" % path)

    def _pump(self):
        try:
            data = self.ser.recv(65536)
            if data:
                self.buf += data
                txt = data.decode("latin1").replace("\r", "")
                self.all += txt
                self.log.write(txt)
                self.log.flush()
        except socket.timeout:
            pass

    def lines(self):
        out = self.pending
        self.pending = []
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            out.append(line.decode("latin1").rstrip("\r"))
        return out

    def wait(self, text, timeout=30):
        end = time.time() + timeout
        got = []
        while time.time() < end:
            self._pump()
            ls = self.lines()
            for i, l in enumerate(ls):
                got.append(l)
                if text in l:
                    self.pending = ls[i + 1:] + self.pending
                    return got
        raise TimeoutError("timeout waiting for %r; got:\n%s" % (text, "\n".join(got[-40:])))

    def collect(self, seconds):
        end = time.time() + seconds
        got = []
        while time.time() < end:
            self._pump()
            got.extend(self.lines())
        return got

    def cmd(self, line, timeout=60):
        self.ser.sendall((line + "\n").encode())
        out = self.wait("READY", timeout)
        return out[:-1]

    def _qmp_read(self):
        while True:
            while b"\n" in self.qbuf:
                line, self.qbuf = self.qbuf.split(b"\n", 1)
                msg = json.loads(line)
                if "event" in msg:
                    continue
                return msg
            self.qbuf += self.qmp.recv(65536)

    def _qmp_cmd(self, obj):
        self.qmp.sendall((json.dumps(obj) + "\n").encode())
        return self._qmp_read()

    def qmp_cmd(self, name, **args):
        r = self._qmp_cmd({"execute": name, "arguments": args})
        if "error" in r:
            raise RuntimeError("qmp %s: %s" % (name, r["error"]))
        return r.get("return")

    def hmp(self, line):
        return self.qmp_cmd("human-monitor-command", **{"command-line": line})

    def close(self):
        try:
            self.proc.kill()
            self.proc.wait(5)
        except Exception:
            pass
        self.log.close()


def disk(path, size_mb=64):
    if not os.path.exists(path):
        subprocess.check_call(["qemu-img", "create", "-f", "raw", path, "%dM" % size_mb],
                              stdout=subprocess.DEVNULL)
    return path
