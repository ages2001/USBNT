#!/bin/bash
# Builds the user mode tools with MinGW-w64 (no C runtime, NT 3.51 compatible)
set -e
cd "$(dirname "$0")"
mkdir -p out
CC=i686-w64-mingw32-gcc
CF="-O2 -march=i486 -mtune=generic -std=gnu89 -Wall -Wno-unused-parameter -fno-builtin -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -mno-stack-arg-probe -mno-sse -D_X86_=1"
LF="-nostdlib -Wl,--entry,_Entry@0 -Wl,--major-subsystem-version,3,--minor-subsystem-version,10 -Wl,--major-os-version,3,--minor-os-version,10 -Wl,--no-insert-timestamp -s"
T=../tools
$CC $CF $T/usbmon.c $T/devmap.c -o out/usbmon.exe $LF -mwindows -lkernel32 -luser32 -ladvapi32
$CC $CF $T/usbtree.c $T/devmap.c -o out/usbtree.exe $LF -mwindows -lkernel32 -luser32
$CC $CF $T/usbeject.c -o out/usbeject.exe $LF -mconsole -lkernel32 -luser32
$CC $CF $T/usbinst.c -o out/usbinst.exe $LF -mconsole -lkernel32 -luser32 -ladvapi32
$CC $CF $T/wheeltst.c -o out/wheeltst.exe $LF -mwindows -lkernel32 -luser32
rm -f out/usbwheel.exe
for f in usbmon usbtree usbeject usbinst wheeltst; do python3 fixexe.py out/$f.exe; done
for f in usbmon usbtree usbeject usbinst wheeltst; do
  if i686-w64-mingw32-objdump -d out/$f.exe | grep -q 'cmov\|fcomi\|fucomi'; then echo "P6 instructions in $f"; exit 1; fi
done
