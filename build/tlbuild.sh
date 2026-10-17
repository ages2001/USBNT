#!/bin/bash
# Builds the user mode tools with MinGW-w64 (no C runtime, NT 3.51 compatible)
set -e
cd "$(dirname "$0")"
mkdir -p out
CC=i686-w64-mingw32-gcc
CF="-O2 -march=i486 -mtune=generic -std=gnu89 -Wall -Werror=frame-larger-than=3000 -Wno-unused-parameter -fno-builtin -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -mno-stack-arg-probe -mno-sse -D_X86_=1"
LF="-nostdlib -Wl,--entry,_Entry@0 -Wl,--major-subsystem-version,3,--minor-subsystem-version,10 -Wl,--major-os-version,3,--minor-os-version,10 -Wl,--no-insert-timestamp -s"
T=../tools
# icons (tools/res/mkicons.py draws them) and the pictures of the usbtree list
RC=i686-w64-mingw32-windres
for r in usbtree usbmon usbcam usbinst usbeject wheeltst; do
  (cd $T/res && $RC -O coff $r.rc -o ../../build/out/$r.res.o)
done
$CC $CF $T/usbmon.c $T/devmap.c out/usbmon.res.o -o out/usbmon.exe $LF -mwindows -lkernel32 -luser32 -lgdi32 -ladvapi32
$CC $CF $T/usbtree.c $T/devmap.c $T/fsdetect.c $T/pnpeject.c out/usbtree.res.o -o out/usbtree.exe $LF -mwindows -lkernel32 -luser32 -lgdi32 -lcomdlg32
$CC $CF $T/usbeject.c $T/devmap.c $T/pnpeject.c out/usbeject.res.o -o out/usbeject.exe $LF -mconsole -lkernel32 -luser32
$CC $CF $T/usbinst.c $T/pnpinst.c out/usbinst.res.o -o out/usbinst.exe $LF -mconsole -lkernel32 -luser32 -ladvapi32
$CC $CF $T/wheeltst.c out/wheeltst.res.o -o out/wheeltst.exe $LF -mwindows -lkernel32 -luser32 -lgdi32
$CC $CF $T/usbcam.c $(ls $T/jpegdec.c $T/avi.c 2>/dev/null) out/usbcam.res.o -o out/usbcam.exe $LF -mwindows -lkernel32 -luser32 -lgdi32 -lcomdlg32
# Video for Windows capture driver (DirectShow sees it on 2000 / XP)
(cd $T/res && $RC -O coff usbntcap.rc -o ../../build/out/usbntcap.res.o)
$CC $CF $T/usbntcap.c $T/jpegdec.c out/usbntcap.res.o $T/usbntcap.def -o out/usbntcap.dll -shared -nostdlib \
  -Wl,--entry,_DllMain@12 -Wl,--enable-stdcall-fixup -Wl,--major-subsystem-version,3,--minor-subsystem-version,10 \
  -Wl,--no-insert-timestamp -s -lkernel32 -luser32 -ladvapi32 -lwinmm -lgcc
python3 fixexe.py out/usbntcap.dll
# test helpers for the NT guest tests (not shipped)
i686-w64-mingw32-gcc -O2 -march=i486 -Wall -Wno-unused-parameter ../test/apps/captest.c -o out/captest.exe -mconsole -s \
  -lvfw32 -lole32 -loleaut32 -lstrmiids -luuid
$CC $CF ../test/apps/fstest.c -o out/fstest.exe $LF -mconsole -lkernel32 -luser32 -ladvapi32
$CC $CF ../test/apps/inptest.c -o out/inptest.exe $LF -mconsole -lkernel32 -luser32 -lgdi32
rm -f out/usbwheel.exe
for f in usbmon usbtree usbeject usbinst wheeltst usbcam fstest inptest; do python3 fixexe.py out/$f.exe; done
for f in usbmon usbtree usbeject usbinst wheeltst usbcam; do
  if i686-w64-mingw32-objdump -d out/$f.exe | grep -q 'cmov\|fcomi\|fucomi'; then echo "P6 instructions in $f"; exit 1; fi
done
