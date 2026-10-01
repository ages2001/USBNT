"""Creates the disk images the NT tests use: stick16.img (64 MB, label
USBSTICK, HELLO.TXT), stick2.img (32 MB, label SECOND, WORLD.TXT) and
cd.iso, all in /tmp/usbt."""
import os, subprocess

W = "/tmp/usbt"


def stick(path, mb, label, name, text):
    if os.path.exists(path):
        return
    with open(path, "wb") as f:
        f.truncate(mb << 20)
    subprocess.run(["sfdisk", "-q", path], input=b"63,,6,*\n", check=True)
    part = "%s@@32256" % path
    subprocess.check_call(["mformat", "-i", part, "-v", label, "-T", str((mb << 11) - 63), "-h", "64", "-s", "32", "-H", "63", "::"])
    tmp = os.path.join(W, name)
    open(tmp, "wb").write(text)
    subprocess.check_call(["mcopy", "-i", part, tmp, "::/" + name.upper()])
    os.unlink(tmp)


def make():
    os.makedirs(W, exist_ok=True)
    stick(os.path.join(W, "stick16.img"), 64, "USBSTICK", "hello.txt", b"hello from usb\n")
    stick(os.path.join(W, "stick2.img"), 32, "SECOND", "world.txt", b"second disk\n")
    iso = os.path.join(W, "cd.iso")
    if not os.path.exists(iso):
        root = os.path.join(W, "cdroot")
        os.makedirs(root, exist_ok=True)
        open(os.path.join(root, "README.TXT"), "wb").write(b"usbnt test cd\r\n")
        subprocess.check_call(["genisoimage", "-quiet", "-V", "USBNTCD", "-o", iso, root])


if __name__ == "__main__":
    make()
