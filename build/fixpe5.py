"""Sets the PE checksum and verifies that every import exists in Windows 2000 (i386) or XP x64 (amd64)."""
import os, sys, pefile
path, arch = sys.argv[1], sys.argv[2]
pe = pefile.PE(path)
here = os.path.dirname(os.path.abspath(__file__))
v = "w2k" if arch == "i386" else "xp64"
exports = {}
for dll in ("ntoskrnl.exe", "hal.dll", "portcls.sys", "ks.sys", "usbd.sys", "hidclass.sys"):
    exports[dll] = set(open(os.path.join(here, v, dll.split(".")[0] + ".exp")).read().split())
bad = []
for imp in pe.DIRECTORY_ENTRY_IMPORT:
    dll = imp.dll.decode().lower()
    for s in imp.imports:
        name = s.name.decode() if s.name else "#%d" % s.ordinal
        if dll not in exports or name not in exports[dll]:
            bad.append("%s!%s" % (dll, name))
if bad:
    print("imports missing in %s:" % v, ", ".join(bad))
    sys.exit(1)
if pe.FILE_HEADER.Characteristics & 0x2000:
    print("image has the DLL flag set")
    sys.exit(1)
if not pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size:
    print("image has no relocations")
    sys.exit(1)
pe.OPTIONAL_HEADER.CheckSum = pe.generate_checksum()
pe.write(path)
n = sum(len(i.imports) for i in pe.DIRECTORY_ENTRY_IMPORT)
print("%s: %d bytes, %d imports, all present in %s" % (path, os.path.getsize(path), n,
      "Windows 2000" if v == "w2k" else "Windows XP x64"))
