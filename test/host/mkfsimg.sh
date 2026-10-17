set -e
mk() { rm -f $1; truncate -s ${2}M $1; }
# superfloppies
mk fat12.img 2; mkfs.vfat -F 12 -n F12 fat12.img >/dev/null
mk fat16.img 64; mkfs.vfat -F 16 -n F16 fat16.img >/dev/null
mk fat32.img 300; mkfs.vfat -F 32 -n F32 fat32.img >/dev/null
mk fat32s.img 40; mkfs.vfat -F 32 -n SMALL32 fat32s.img >/dev/null
mk ntfs.img 64; mkntfs -F -Q -q -L NTFSVOL ntfs.img
mk exfat.img 64; mkfs.exfat -L EXVOL exfat.img >/dev/null
mk ext2.img 32; mke2fs -q -t ext2 -L E2 ext2.img
mk ext3.img 32; mke2fs -q -t ext3 -L E3 ext3.img
mk ext4.img 64; mke2fs -q -t ext4 -L E4 ext4.img
mk btrfs.img 160; mkfs.btrfs -q -L BTR btrfs.img
mk xfs.img 320; mkfs.xfs -q -L XF xfs.img
mk hfsp.img 32; mkfs.hfsplus -v HFSP hfsp.img >/dev/null
genisoimage -quiet -V ISOVOL -o iso.img /etc/hostname
