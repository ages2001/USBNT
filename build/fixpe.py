"""Sets the PE checksum and verifies that every import exists in NT 3.1, 3.5, 3.51 and 4.0."""
import os, sys, pefile
path = sys.argv[1]
pe = pefile.PE(path)
here = os.path.dirname(os.path.abspath(__file__))
VERSIONS = ("nt31rtm", "nt31", "nt35", "nt351", "nt4")
bad = []
for v in VERSIONS:
    exports = {}
    for dll, f in (("ntoskrnl.exe", "ntoskrnl.exp"), ("hal.dll", "hal.exp")):
        exports[dll] = set(open(os.path.join(here, v, f)).read().split())
    for imp in pe.DIRECTORY_ENTRY_IMPORT:
        dll = imp.dll.decode().lower()
        for s in imp.imports:
            name = s.name.decode() if s.name else "#%d" % s.ordinal
            if dll not in exports or name not in exports[dll]:
                bad.append("%s!%s (%s)" % (dll, name, v))
if bad:
    print("imports missing:", ", ".join(bad))
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
print("%s: %d bytes, %d imports, all present in NT 3.1-4.0" % (path, os.path.getsize(path), n))
