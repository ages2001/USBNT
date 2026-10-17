#!/bin/bash
# Builds the Windows 2000 / XP drivers with MinGW-w64: out/2kxp/i386/usbnt.sys and out/2kxp/amd64/usbnt.sys
set -e
cd "$(dirname "$0")"
SRC=../src
CORE="usbutil dmapool usbcore usbhub usbmsc usbhid hidparse hcxhci hcehci hcuhci hcohci usbaudio sdcard sdhci rtsxusb rtsxpci usbvideo i2chid acpi aml gpio ptp ps2pad nvmehp tbt smbus smbtp smbtpdec"
WDM="ntos ntinput ntscsi ntaudio wdmmain wdmbus wdmflt wdmps2 wdmtree usbdi"
for n in $(grep -oh 'OsGetConfig("[A-Za-z0-9]*"' $SRC/*.c | sed 's/OsGetConfig("//;s/"//' | sort -u); do
  grep -q "{ \"$n\"," $SRC/ntos.c || { echo "parameter $n is missing in the Config table of ntos.c"; exit 1; }
done
COMMON="-c -O2 -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wdeclaration-after-statement \
 -fno-builtin -ffreestanding -fno-stack-protector -mno-stack-arg-probe -mno-sse -mno-mmx -DUSB_WDM=1 -DNDEBUG \
 -I/usr/share/mingw-w64/include/ddk -I$SRC -Werror=implicit-function-declaration -Werror=frame-larger-than=3000"
for arch in i386 amd64; do
  O=out/2kxp/$arch
  mkdir -p $O; rm -f $O/*.o
  if [ $arch = i386 ]; then
    CC=i686-w64-mingw32-gcc; CF="$COMMON -march=i586 -mtune=generic -fno-asynchronous-unwind-tables -D_X86_=1"
    ENTRY=_DriverEntry@8; OSV="5,--minor-subsystem-version,0 -Wl,--major-os-version,5,--minor-os-version,0"
    OBJDUMP=i686-w64-mingw32-objdump
  else
    CC=x86_64-w64-mingw32-gcc; CF="$COMMON -Wno-array-bounds -mno-red-zone -mcmodel=small -D_AMD64_=1 -D_WIN64=1"
    ENTRY=DriverEntry; OSV="5,--minor-subsystem-version,2 -Wl,--major-os-version,5,--minor-os-version,2"
    OBJDUMP=x86_64-w64-mingw32-objdump
  fi
  OBJS=""
  for f in $CORE $WDM; do
    $CC $CF $SRC/$f.c -o $O/$f.o
    OBJS="$OBJS $O/$f.o"
  done
  $CC -nostdlib -o $O/usbnt.sys $OBJS \
    -Wl,--subsystem,native -Wl,--image-base,0x10000 -Wl,--entry,$ENTRY \
    -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -Wl,--enable-reloc-section \
    -Wl,--major-subsystem-version,$OSV -Wl,--major-image-version,1 \
    -Wl,--no-insert-timestamp -Wl,-s -lntoskrnl -lhal -lgcc
  python3 fixpe5.py $O/usbnt.sys $arch
done
# the 64 bit usbinst.exe: SetupAPI installs devices on 64 bit Windows only from a 64 bit program
CC=x86_64-w64-mingw32-gcc
(cd ../tools/res && x86_64-w64-mingw32-windres -O coff usbinst.rc -o ../../build/out/2kxp/amd64/usbinst.res.o)
$CC -O2 -std=gnu89 -Wall -Wno-unused-parameter -fno-builtin -ffreestanding -fno-stack-protector -mno-stack-arg-probe \
  -fno-asynchronous-unwind-tables ../tools/usbinst.c ../tools/pnpinst.c out/2kxp/amd64/usbinst.res.o -o out/2kxp/amd64/usbinst.exe \
  -nostdlib -Wl,--entry,Entry -Wl,--major-subsystem-version,5,--minor-subsystem-version,2 \
  -Wl,--major-os-version,5,--minor-os-version,2 -Wl,--no-insert-timestamp -s -mconsole -lkernel32 -luser32 -ladvapi32
(cd ../tools/res && x86_64-w64-mingw32-windres -O coff usbntcap.rc -o ../../build/out/2kxp/amd64/usbntcap.res.o)
$CC -O2 -std=gnu89 -Wall -Wno-unused-parameter -fno-builtin -ffreestanding -fno-stack-protector -mno-stack-arg-probe \
  -fno-asynchronous-unwind-tables ../tools/usbntcap.c ../tools/jpegdec.c out/2kxp/amd64/usbntcap.res.o ../tools/usbntcap.def \
  -o out/2kxp/amd64/usbntcap.dll -shared -nostdlib -Wl,--entry,DllMain -Wl,--major-subsystem-version,5,--minor-subsystem-version,2 \
  -Wl,--no-insert-timestamp -s -lkernel32 -luser32 -ladvapi32 -lwinmm -lgcc
rm -f out/2kxp/amd64/usbinst.res.o out/2kxp/amd64/usbntcap.res.o
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-parameter ../test/apps/captest.c -o out/2kxp/amd64/captest.exe -mconsole -s \
  -lvfw32 -lole32 -loleaut32 -lstrmiids -luuid
python3 mkinf.py
ls -la out/2kxp out/2kxp/i386 out/2kxp/amd64
