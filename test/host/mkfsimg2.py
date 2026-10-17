import struct, subprocess, os, zlib, uuid
def blank(p, mb):
    with open(p,'wb') as f: f.truncate(mb<<20)
# HPFS: boot sector + super block at sector 16 + spare at 17
blank('hpfs.img', 8)
with open('hpfs.img','r+b') as f:
    bs = bytearray(512); bs[0:3]=b'\xeb\x3c\x90'; bs[3:11]=b'IBM 4.0 '; bs[0x0b:0x0d]=struct.pack('<H',512)
    bs[0x26]=0x29; bs[0x2b:0x36]=b'HPFSVOL    '; bs[0x36:0x3e]=b'HPFS    '; bs[510:512]=b'\x55\xaa'
    f.write(bs); f.seek(16*512); sb=bytearray(512); sb[0:8]=struct.pack('<II',0xF995E849,0xFA53E9C5); sb[8]=2; sb[9]=2; f.write(sb)
# ReFS 3.4 boot sector
blank('refs.img', 64)
with open('refs.img','r+b') as f:
    b=bytearray(512); b[3:11]=b'ReFS\0\0\0\0'; b[0x10:0x14]=b'FSRS'; b[0x14:0x16]=struct.pack('<H',0x200)
    b[0x18:0x20]=struct.pack('<Q',131072); b[0x20:0x24]=struct.pack('<I',512); b[0x24:0x28]=struct.pack('<I',8); b[0x28]=3; b[0x29]=4
    f.write(b)
# NTFS 1.2: patch the $Volume version of a fresh volume
import shutil; shutil.copy('ntfs.img','ntfs12.img')
d=bytearray(open('ntfs12.img','rb').read())
bps=struct.unpack_from('<H',d,0xb)[0]; spc=d[0xd]; mft=struct.unpack_from('<Q',d,0x30)[0]; cpr=struct.unpack_from('<b',d,0x40)[0]
rs = 1<<(-cpr) if cpr<0 else cpr*bps*spc
o = mft*bps*spc + 3*rs
a = struct.unpack_from('<H',d,o+0x14)[0]
while True:
    t,l = struct.unpack_from('<II',d,o+a)
    if t==0xffffffff: break
    if t==0x70:
        vo=struct.unpack_from('<H',d,o+a+0x14)[0]; d[o+a+vo+8]=1; d[o+a+vo+9]=2; break
    a+=l
open('ntfs12.img','wb').write(d)
def mbr_disk(out, parts, total_mb):
    """parts: list of (image, type)"""
    blank(out, total_mb)
    lba=2048; ents=b''
    with open(out,'r+b') as f:
        for img,t in parts:
            data=open(img,'rb').read(); n=len(data)//512
            f.seek(lba*512); f.write(data)
            ents+=struct.pack('<B3sB3sII',0,b'\0\0\0',t,b'\0\0\0',lba,n); lba+=n+2048
        f.seek(446); f.write(ents.ljust(64,b'\0')); f.write(b'\x55\xaa')
mbr_disk('mbr_mix.img',[('fat16.img',6),('ntfs.img',7),('ext4.img',0x83),('exfat.img',7)], 300)
# extended partition with two logicals
blank('mbr_ext.img', 200)
with open('mbr_ext.img','r+b') as f:
    a=open('fat12.img','rb').read(); b=open('ext2.img','rb').read()
    f.seek(2048*512); f.write(open('fat32s.img','rb').read())
    ext_start=2048+len(open('fat32s.img','rb').read())//512+2048
    # EBR1 at ext_start: logical at +2048 (fat12), link to EBR2
    l1=len(a)//512; ebr2_rel=2048+l1+2048; l2=len(b)//512
    e1=bytearray(512); struct.pack_into('<B3sB3sII',e1,446,0,b'\0\0\0',1,b'\0\0\0',2048,l1)
    struct.pack_into('<B3sB3sII',e1,462,0,b'\0\0\0',5,b'\0\0\0',ebr2_rel,2048+l2); e1[510:]=b'\x55\xaa'
    f.seek(ext_start*512); f.write(e1); f.seek((ext_start+2048)*512); f.write(a)
    e2=bytearray(512); struct.pack_into('<B3sB3sII',e2,446,0,b'\0\0\0',0x83,b'\0\0\0',2048,l2); e2[510:]=b'\x55\xaa'
    f.seek((ext_start+ebr2_rel)*512); f.write(e2); f.seek((ext_start+ebr2_rel+2048)*512); f.write(b)
    m=bytearray(64); struct.pack_into('<B3sB3sII',m,0,0,b'\0\0\0',0xc,b'\0\0\0',2048,len(open('fat32s.img','rb').read())//512)
    struct.pack_into('<B3sB3sII',m,16,0,b'\0\0\0',0x0f,b'\0\0\0',ext_start,ebr2_rel+2048+l2+10)
    f.seek(446); f.write(m); f.write(b'\x55\xaa')
# GPT: btrfs + refs + hpfs-ish? (ntfs) with names
def gpt_disk(out, parts, total_mb):
    blank(out,total_mb); tot=(total_mb<<20)//512
    with open(out,'r+b') as f:
        pm=bytearray(512); struct.pack_into('<B3sB3sII',pm,446,0,b'\0\0\0',0xee,b'\0\0\0',1,min(tot-1,0xffffffff)); pm[510:]=b'\x55\xaa'; f.write(pm)
        ents=bytearray(128*128); lba=2048
        for i,(img,name) in enumerate(parts):
            data=open(img,'rb').read(); n=len(data)//512
            f.seek(lba*512); f.write(data)
            e=uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7').bytes_le+uuid.uuid4().bytes_le+struct.pack('<QQQ',lba,lba+n-1,0)+name.encode('utf-16-le').ljust(72,b'\0')
            ents[i*128:(i+1)*128]=e; lba+=n+2048
        h=bytearray(92); h[0:8]=b'EFI PART'; struct.pack_into('<IIIIQQQQ16sQIII',h,8,0x10000,92,0,0,1,tot-1,34,tot-34,uuid.uuid4().bytes_le,2,128,128,zlib.crc32(ents))
        struct.pack_into('<I',h,16,zlib.crc32(h)); f.seek(512); f.write(h.ljust(512,b'\0')); f.seek(1024); f.write(ents)
gpt_disk('gpt.img',[('btrfs.img','Linux data'),('refs.img','ReFS vol'),('ntfs12.img','old ntfs'),('xfs.img','xfs')], 700)
mbr_disk('mbr_hpfs.img',[('hpfs.img',7),('hfsp.img',0xaf)],64)
