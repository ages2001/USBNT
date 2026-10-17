set -e
cd "$(dirname "$0")"
HCS="${HCS:-xhci}"
DEFS=""
SRCS="os.c main.c audio.c i2c.c smbhar.c video.c ps2.c stubhc.c ../src/usbutil.c ../src/dmapool.c ../src/usbcore.c ../src/usbhub.c ../src/usbmsc.c ../src/usbhid.c ../src/hidparse.c ../src/usbaudio.c ../src/sdcard.c ../src/sdhci.c ../src/rtsxusb.c ../src/rtsxpci.c ../src/usbvideo.c ../src/i2chid.c ../src/acpi.c ../src/aml.c ../src/gpio.c ../src/ptp.c ../src/ps2pad.c ../src/nvmehp.c ../src/tbt.c ../src/smbus.c ../src/smbtp.c ../src/smbtpdec.c"
for h in $HCS; do SRCS="$SRCS ../src/hc$h.c"; U=$(echo $h | tr a-z A-Z); DEFS="$DEFS -DHAVE_$U"; done
CF="-m32 -ffreestanding -fno-builtin -nostdlib -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wdeclaration-after-statement -DUSB_HARNESS $DEFS -fno-pic -fno-stack-protector -mno-sse -mno-mmx -I../src"
rm -rf obj; mkdir -p obj
gcc $CF -c boot.S -o obj/boot.o
OBJS="obj/boot.o"
for f in $SRCS; do o=obj/$(basename $f .c).o; gcc $CF -c $f -o $o; OBJS="$OBJS $o"; done
ld -m elf_i386 --no-warn-rwx-segments -z noexecstack -T link.ld -o kernel.elf $OBJS
echo built kernel.elf
