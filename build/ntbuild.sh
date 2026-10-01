#!/bin/bash
# Builds usbnt.sys with MinGW-w64 (test builds; the release build uses the NT4 DDK)
set -e
cd "$(dirname "$0")"
SRC=../src
OUT=out
mkdir -p $OUT; rm -f $OUT/*.o
CC=i686-w64-mingw32-gcc
CF="-c -O2 -march=i486 -mtune=generic -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wdeclaration-after-statement \
 -fno-builtin -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -mno-stack-arg-probe -mno-sse -mno-mmx \
 -D_X86_=1 -DNDEBUG -I/usr/share/mingw-w64/include/ddk -I$SRC"
OBJS=""
for f in usbutil dmapool usbcore usbhub usbmsc usbhid hidparse hcxhci hcehci hcuhci hcohci ntos ntmain ntscsi ntinput; do
  $CC $CF $SRC/$f.c -o $OUT/$f.o
  OBJS="$OBJS $OUT/$f.o"
done
$CC -nostdlib -o $OUT/usbnt.sys $OBJS \
  -Wl,--subsystem,native -Wl,--image-base,0x10000 -Wl,--entry,_DriverEntry@8 \
  -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -Wl,--enable-reloc-section \
  -Wl,--major-subsystem-version,3,--minor-subsystem-version,10 \
  -Wl,--major-os-version,3,--minor-os-version,10 -Wl,--major-image-version,1 \
  -Wl,--no-insert-timestamp -Wl,-s \
  -lntoskrnl -lhal
python3 fixpe.py $OUT/usbnt.sys
if i686-w64-mingw32-objdump -d $OUT/usbnt.sys | grep -q 'cmov\|fcomi\|fucomi'; then echo "P6 instructions found"; exit 1; fi
