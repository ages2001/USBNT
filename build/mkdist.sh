#!/bin/bash
# Builds the release: bin/ (the binaries kept in the repository), dist/USBNT-<ver>/,
# dist/USBNT-<ver>.zip, the source archive dist/usbnt-src-<ver>.zip (current files) and the
# driver disks for text-mode setup dist/USBNTNT.IMG, USBNT2K.IMG, USBNT64.IMG (1.44 MB floppies,
# one per Windows family, every file in the root; also in dist/USBNT-<ver>/OEMDISK/NT, 2KXP, XP64
# for nLite and the like).
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/^#define USBNT_VERSION_STR "\([0-9.]*\)"$/\1/p' ../src/usbntver.h)
[ -n "$VER" ] || { echo "Missing USBNT product version" >&2; exit 1; }
for existing in ../dist/USBNT-$VER ../dist/USBNT-$VER.zip ../dist/usbnt-src-$VER.zip; do
    if [ -e "$existing" ]; then
        echo "Preserving existing release output: $existing" >&2
        exit 1
    fi
done
bash ntbuild.sh
bash tlbuild.sh
bash wdmbuild.sh
python3 mkoem.py
cd ..
for f in usbnt.sys usbinst.exe usbmon.exe usbtree.exe usbeject.exe usbcam.exe usbntcap.dll wheeltst.exe; do
    cp build/out/$f bin/$f
done
# Windows 2000 / XP: INF files, the x86 and x64 drivers, the 64 bit installer and capture driver
mkdir -p bin/2kxp/i386 bin/2kxp/amd64
cp build/out/2kxp/usbnt.inf build/out/2kxp/usbntkbd.inf build/out/2kxp/usbntmou.inf bin/2kxp/
cp build/out/2kxp/i386/usbnt.sys bin/2kxp/i386/
cp build/out/2kxp/amd64/usbnt.sys build/out/2kxp/amd64/usbinst.exe build/out/2kxp/amd64/usbntcap.dll bin/2kxp/amd64/
mkdir -p dist/USBNT-$VER
cp -r bin/* README.md CHANGELOG.md LICENSE dist/USBNT-$VER/
# text-mode setup (TXTSETUP.OEM): the floppy images and their files
cp build/out/USBNTNT.IMG build/out/USBNT2K.IMG build/out/USBNT64.IMG dist/USBNT-$VER/
cp -r build/out/oem dist/USBNT-$VER/OEMDISK
(cd dist && zip -q -r USBNT-$VER.zip USBNT-$VER)
python3 build/archive_source.py "$VER" "dist/usbnt-src-$VER.zip"
ls -laR dist/USBNT-$VER; ls -la dist
