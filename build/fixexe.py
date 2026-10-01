"""Verifies that every import of a user mode tool exists in NT 3.1, 3.5, 3.51 and 4.0."""
import os, sys, pefile
here = os.path.dirname(os.path.abspath(__file__))
VERSIONS = ("nt31rtm", "nt31", "nt35", "nt351", "nt4")
pe = pefile.PE(sys.argv[1])
bad = []
for imp in pe.DIRECTORY_ENTRY_IMPORT:
    dll = imp.dll.decode().lower()
    for v in VERSIONS:
        p = os.path.join(here, v, dll.split(".")[0] + ".exp")
        if not os.path.exists(p):
            bad.append("%s (%s: no export list)" % (dll, v))
            continue
        names = set(open(p).read().split())
        for s in imp.imports:
            n = s.name.decode()
            if n not in names:
                bad.append("%s!%s (%s)" % (dll, n, v))
if bad:
    print(sys.argv[1], "missing:", ", ".join(bad)); sys.exit(1)
pe.OPTIONAL_HEADER.CheckSum = pe.generate_checksum(); pe.write(sys.argv[1])
print("%s: %d bytes, imports OK on NT 3.1-4.0" % (sys.argv[1], os.path.getsize(sys.argv[1])))
