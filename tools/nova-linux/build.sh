#!/bin/bash
# Nova-Linux 1.0 Sun - self-contained live/installer ISO
# Linux 6.8 kernel + BusyBox userland + self-made install wizard (novainstall)
set -e
cd /tmp/nlbuild
R=rootfs
rm -rf $R iso
mkdir -p $R/{bin,sbin,etc,proc,sys,dev,dev/pts,mnt,target,tmp,usr/bin,usr/sbin,lib,boot/extlinux}

BB=bb/usr/bin/busybox
mkdir -p $R/bin
cp $BB $R/bin/busybox
for a in $($BB --list); do
  [ "$a" = "busybox" ] && continue
  ln -sf /bin/busybox $R/bin/$a
done

# extlinux + shared libs (for bootloader install on target disk)
mkdir -p $R/usr/lib/x86_64-linux-gnu $R/lib/x86_64-linux-gnu $R/sbin $R/usr/lib/syslinux/modules/bios
dpkg-deb -x extlinux*.deb e1 && dpkg-deb -x syslinux-common*.deb s1
cp e1/usr/sbin/extlinux $R/sbin/ 2>/dev/null || cp e1/usr/bin/extlinux $R/sbin/
find e1 s1 -type f | head -20
cp s1/usr/lib/syslinux/modules/bios/*.c32 $R/usr/lib/syslinux/modules/bios/ 2>/dev/null || true
cp s1/usr/lib/syslinux/mbr/mbr.bin $R/usr/lib/mbr.bin 2>/dev/null || find s1 -path "*mbr/mbr.bin" -exec cp {} $R/usr/lib/mbr.bin \;
cp s1/usr/lib/syslinux/mbr/altmbr.bin $R/usr/lib/altmbr.bin 2>/dev/null || find s1 -path "*mbr/altmbr.bin" -exec cp {} $R/usr/lib/altmbr.bin \;
find e1 -name "*.so*" -exec cp {} $R/lib/x86_64-linux-gnu/ \; 2>/dev/null || true
for lib in $(ldd $R/sbin/extlinux 2>/dev/null | awk '/=>/{print $3}'); do
  [ -f "$lib" ] && cp $lib $R/lib/x86_64-linux-gnu/
done
LDR=$(ldd $R/sbin/extlinux 2>/dev/null | awk '/ld-linux/{print $1}')
cp $LDR $R/lib/x86_64-linux-gnu/ 2>/dev/null || cp /lib64/ld-linux-x86-64.so.2 $R/lib/x86_64-linux-gnu/
mkdir -p $R/lib64 && ln -sf /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $R/lib64/ld-linux-x86-64.so.2

# sfdisk (needed for dual-boot "use free space" partitioning) + its libs
cp fdk/usr/sbin/sfdisk $R/sbin/ 2>/dev/null || cp /usr/sbin/sfdisk $R/sbin/
for lib in $(ldd $R/sbin/sfdisk 2>/dev/null | awk '/=>/{print $3}'); do
  [ -f "$lib" ] && cp $lib $R/lib/x86_64-linux-gnu/
done

# graphical install wizard (static binary, draws on /dev/fb0)
cp gui/novainstall-gui $R/usr/sbin/novainstall-gui

# kernel modules (subset needed for disks/fs/cd/virtio/input/drm-fb, plus metadata)
MODSRC=mpkg/lib/modules/6.8.0-31-generic
XPSRC=xpkg/lib/modules/6.8.0-31-generic
KM=$R/lib/modules/6.8.0-31-generic/kernel
mkdir -p $KM
for d in fs drivers/ata drivers/scsi drivers/cdrom drivers/virtio drivers/block drivers/md drivers/nvme drivers/usb/storage drivers/hid drivers/input drivers/pcmcia drivers/i2c drivers/gpu/drm/virtio drivers/gpu/drm/ttm drivers/gpu/drm/ast; do
  [ -d $MODSRC/kernel/$d ] && cp -r $MODSRC/kernel/$d $KM/ || true
done
# extra DRM drivers for framebuffer (QEMU std VGA = bochs, qxl, ast)
for d in drivers/gpu/drm/tiny drivers/gpu/drm/qxl drivers/gpu/drm/ast drivers/gpu/drm/scheduler drivers/gpu/drm/ttm drivers/gpu/drm/gud; do
  [ -d $XPSRC/kernel/$d ] && mkdir -p $KM/$(dirname $d) && cp -r $XPSRC/kernel/$d $KM/$(dirname $d)/ || true
done
mkdir -p $KM/drivers/gpu/drm
for f in drm_buddy drm_dma_helper drm_exec drm_gpuvm drm_suballoc_helper drm_mipi_dbi drm_ttm_helper drm_vram_helper; do
  find $XPSRC/kernel/drivers/gpu/drm $MODSRC/kernel/drivers/gpu/drm -maxdepth 1 -name "$f.ko*" -exec cp {} $KM/drivers/gpu/drm/ \; 2>/dev/null || true
done
[ -d $XPSRC/kernel/drivers/video ] && cp -r $XPSRC/kernel/drivers/video $KM/drivers/ || true
cp $MODSRC/modules.builtin* $MODSRC/modules.order $R/lib/modules/6.8.0-31-generic/ 2>/dev/null || true
# decompress modules: this kernel config rejects .ko.zst (invalid module format)
find $R/lib/modules -name "*.ko.zst" | while read m; do
  unzstd -q "$m" -o "${m%.zst}" && rm -f "$m"
done
depmod -b $R 6.8.0-31-generic

# live init (PID 1): root= -> switch_root, else live shell
cat > $R/init <<'EOS'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sys /sys
/bin/busybox mkdir -p /run
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null || /bin/busybox mdev -s
/bin/busybox mount -t devpts devpts /dev/pts 2>/dev/null
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

ROOTDEV=""
for a in $(cat /proc/cmdline); do
  case "$a" in root=*) ROOTDEV="${a#root=}";; quiet) QUIET=1;; esac
done

if [ -n "$ROOTDEV" ] && [ -b "$ROOTDEV" ]; then
  # installed-system boot: mount real root and hand over
  /bin/busybox mount -o rw "$ROOTDEV" /mnt 2>/dev/null || /bin/busybox mount -t ext2 -o rw "$ROOTDEV" /mnt
  if [ -x /mnt/sbin/init ] || [ -f /mnt/sbin/init ]; then
    /bin/busybox echo "Nova-Linux: switch_root to $ROOTDEV"
    /bin/busybox umount /proc /sys /dev/pts /dev 2>/dev/null
    exec /bin/busybox switch_root /mnt /sbin/init
  fi
  /bin/busybox echo "root $ROOTDEV mount failed - falling back to live shell"
fi

# live environment
/bin/busybox mdev -s 2>/dev/null
/bin/busybox dmesg -n 1
# try the graphical installer first (bochs/virtio-gpu/qxl DRM -> /dev/fb0)
for m in bochs qxl virtio-gpu ast vboxvideo; do
  /bin/busybox modprobe $m >/tmp/mp.log 2>&1 || /bin/busybox echo "init: modprobe $m -> $(cat /tmp/mp.log)" > /dev/ttyS0
done
/bin/busybox dmesg | /bin/busybox grep -iE "symbol|disagrees|vermagic|bochs|qxl|ast" | /bin/busybox tail -8 > /dev/ttyS0
/bin/busybox echo "init: dmesg-tail $(/bin/busybox dmesg | /bin/busybox tail -1)" > /dev/ttyS0
n=0
while [ ! -c /dev/fb0 ] && [ $n -lt 5 ]; do /bin/busybox sleep 1; n=$((n+1)); done
/bin/busybox echo "init: fb0=$([ -c /dev/fb0 ] && echo yes || echo no) gui=$([ -x /usr/sbin/novainstall-gui ] && echo yes || echo no) ttyS0" > /dev/ttyS0 2>/dev/null
if [ -c /dev/fb0 ] && [ -x /usr/sbin/novainstall-gui ]; then
  exec /usr/sbin/novainstall-gui
fi
# no framebuffer: fall back to the text wizard
clear
echo ""
echo "==============================================================="
echo "  Nova-Linux 1.0 (Sun) - live environment"
echo "  Linux 6.8.0-31-generic + BusyBox, 100% NovaOS project"
echo "==============================================================="
echo "  install to disk : run 'novainstall'"
echo "  explore         : ls /proc/partitions, lspci-lite, dmesg"
echo "==============================================================="
echo ""
# serial fallback shell (VGA console stays primary)
/bin/busybox setsid /bin/busybox sh -c 'exec /bin/busybox sh </dev/ttyS0 >/dev/ttyS0 2>&1' 2>/dev/null &
exec /bin/busybox setsid /bin/busybox cttyhack /bin/busybox sh
EOS
chmod +x $R/init

# /etc for the INSTALLED system (copied by the wizard)
mkdir -p $R/rootfs-template/{sbin,etc,dev,proc,sys,tmp,mnt,root,home,usr/bin,lib,boot}
cat > $R/rootfs-template/sbin/init <<'EOS'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sys /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t devpts devpts /dev/pts 2>/dev/null
clear
echo ""
echo "==============================================================="
echo "  Nova-Linux 1.0 (Sun) - installed system"
echo "  booted from $(cat /proc/cmdline 2>/dev/null | tr ' ' '\n' | grep ^root= || echo disk)"
echo "==============================================================="
echo ""
/bin/busybox setsid /bin/busybox sh -c 'exec /bin/busybox sh </dev/ttyS0 >/dev/ttyS0 2>&1' 2>/dev/null &
exec /bin/busybox setsid /bin/busybox cttyhack /bin/busybox sh
EOS
chmod +x $R/rootfs-template/sbin/init
cp $R/bin/busybox $R/rootfs-template/bin/busybox 2>/dev/null || { mkdir -p $R/rootfs-template/bin; cp $R/bin/busybox $R/rootfs-template/bin/; }
ln -sf busybox $R/rootfs-template/bin/sh

# ---------------- the installer wizard ----------------
cat > $R/usr/sbin/nova-alongside <<'EOS'
#!/bin/busybox sh
# Nova-Linux "install alongside" - creates a partition in unpartitioned
# space, installs the system there, MBR/bootloader of the existing OS untouched.
echo "Nova-Linux alongside-install (dual boot)"
i=0
for d in /sys/block/*/device; do
  dev=$(echo $d | cut -d/ -f4)
  case "$dev" in loop*|ram*|sr*) continue;; esac
  sz=$(cat /sys/block/$dev/size 2>/dev/null)
  [ -z "$sz" ] && continue
  echo "  /dev/$dev - $((sz * 512 / 1024 / 1024)) MB"
  i=$((i+1))
done
[ $i -eq 0 ] && { echo "no disks found"; exit 1; }
printf "target disk (e.g. sda): "
read TGT
[ -b /dev/$TGT ] || { echo "/dev/$TGT not found"; exit 1; }

# find unpartitioned gap >= 16MB using the sfdisk dump
sfdisk -d /dev/$TGT > /tmp/tbl 2>/dev/null || { echo "sfdisk -d failed"; exit 1; }
FREE=$(awk -v total=$(cat /sys/block/$TGT/size) '
  /^label: gpt/ { gpt=1 }
  /start=/ {
    s=0; sz=0
    for (i=1;i<=NF;i++) {
      if ($i ~ /^start=/) { sub("start=","",$i); sub(",","",$i); s=$i+0 }
      if ($i ~ /^size=/)  { sub("size=","",$i);  sub(",","",$i); sz=$i+0 }
    }
    if (sz>0) { lo[++n]=s; hi[n]=s+sz }
  }
  END {
    cur=2048
    for (i=1;i<=n;i++) {
      for (j=i+1;j<=n;j++) if (lo[j]<lo[i]) {
        t=lo[i]; lo[i]=lo[j]; lo[j]=t; t=hi[i]; hi[i]=hi[j]; hi[j]=t
      }
    }
    for (i=1;i<=n;i++) {
      if (hi[i]<=cur) continue
      if (lo[i]-cur>=32768) { print cur, lo[i]-cur-2048; exit }
      if (hi[i]>cur) cur=hi[i]
    }
    if (total-cur>=32768) print cur, total-cur-2048
  }' /tmp/tbl)
[ -z "$FREE" ] && { echo "no unpartitioned space on /dev/$TGT - use erase mode"; exit 1; }
FSTART=$(echo $FREE | cut -d' ' -f1)
FSIZE=$(echo $FREE | cut -d' ' -f2)
N=$(grep -c "start=" /tmp/tbl)
if grep -q "^label: gpt" /tmp/tbl; then TYPE=linux; else
  TYPE=83
  [ $N -ge 4 ] && { echo "MBR has no free primary slot - use erase mode"; exit 1; }
fi
PNUM=$((N+1))
echo "will create /dev/${TGT}${PNUM}: start=$FSTART sectors=$FSIZE ($((FSIZE*512/1024/1024)) MB)"
printf "proceed? (yes/no): "
read ANS
[ "$ANS" = "yes" ] || { echo "aborted"; exit 1; }

echo "[1/6] creating partition"
echo "/dev/${TGT}${PNUM} : start=$FSTART, size=$FSIZE, type=$TYPE" >> /tmp/tbl
sfdisk /dev/$TGT < /tmp/tbl >/dev/null 2>&1 || { echo "sfdisk failed"; exit 1; }
sleep 1; mdev -s 2>/dev/null
echo "[2/6] formatting /dev/${TGT}${PNUM} (ext2)"
mke2fs -F /dev/${TGT}${PNUM} >/dev/null 2>&1 || { echo "mkfs failed"; exit 1; }
echo "[3/6] copying system"
mkdir -p /mnt/target
mount -t ext2 /dev/${TGT}${PNUM} /mnt/target || { echo "mount failed"; exit 1; }
cp -a /rootfs-template/. /mnt/target/
echo "[4/6] kernel + initramfs"
mkdir -p /mnt/target/boot/extlinux
cp /usr/lib/syslinux/modules/bios/ldlinux.c32 /mnt/target/boot/extlinux/ 2>/dev/null
cp /boot/vmlinuz /mnt/target/boot/vmlinuz
cp /boot/initramfs-live.gz /mnt/target/boot/initramfs.gz
cat > /mnt/target/boot/extlinux/extlinux.conf <<CONF
DEFAULT nova-linux
TIMEOUT 30
LABEL nova-linux
  KERNEL /boot/vmlinuz
  INITRD /boot/initramfs.gz
  APPEND root=/dev/${TGT}${PNUM} console=ttyS0 console=tty0
CONF
echo "[5/6] bootloader (partition VBR only - MBR untouched)"
extlinux --install /mnt/target/boot/extlinux 2>&1 | tail -1
echo "[6/6] sync"
umount /mnt/target
sync
echo ""
echo "install complete: /dev/${TGT}${PNUM}"
echo "your existing system/MBR was NOT modified."
echo "to add Nova-Linux to your existing boot menu, boot it and run 'update-grub'."
echo "you can also pick the partition from your firmware boot menu."
EOS
chmod +x $R/usr/sbin/nova-alongside

cat > $R/usr/sbin/novainstall <<'EOS'
#!/bin/busybox sh
# Nova-Linux install wizard (self-made, no external deps)
echo ""
echo "==============================================================="
echo "  Nova-Linux 1.0 (Sun) - install wizard"
echo "==============================================================="
echo ""
echo "Install modes:"
echo "  1) Erase entire disk & install"
echo "  2) Install alongside existing OS (use free space)"
echo "  3) Cancel"
printf "mode [1-3]: "
read MODE
case "$MODE" in
  3) echo "aborted"; exit 0;;
  2) exec /usr/sbin/nova-alongside;;
esac
echo ""
echo "Available disks:"
i=0
for d in /sys/block/*/device; do
  dev=$(echo $d | cut -d/ -f4)
  case "$dev" in loop*|ram*|sr*) continue;; esac
  sz=$(cat /sys/block/$dev/size 2>/dev/null)
  [ -z "$sz" ] && continue
  echo "  /dev/$dev - $((sz * 512 / 1024 / 1024)) MB"
  i=$((i+1))
done
[ $i -eq 0 ] && { echo "no disks found"; exit 1; }
echo ""
printf "target disk (e.g. sda): "
read TGT
[ -b /dev/$TGT ] || { echo "/dev/$TGT not found"; exit 1; }
echo ""
echo "WARNING: ALL data on /dev/$TGT will be ERASED."
printf "proceed? (yes/no): "
read ANS
[ "$ANS" = "yes" ] || { echo "aborted"; exit 1; }

echo ""
echo "[1/6] partitioning /dev/$TGT (single ext2 partition, full disk)"
echo -e "o\nn\np\n1\n\n\nw" | fdisk /dev/$TGT >/dev/null 2>&1
sleep 1

echo "[2/6] formatting /dev/${TGT}1 (ext2)"
mkdir -p /mnt/target
n=0
while [ ! -b /dev/${TGT}1 ] && [ $n -lt 10 ]; do mdev -s 2>/dev/null; sleep 1; n=$((n+1)); done
[ -b /dev/${TGT}1 ] || { echo "partition not visible after fdisk"; exit 1; }
mke2fs -F /dev/${TGT}1 >/dev/null 2>&1 || { echo "mkfs failed"; exit 1; }

echo "[3/6] mounting and copying system"
mount -t ext2 /dev/${TGT}1 /mnt/target || { echo "mount failed"; exit 1; }
cp -a /rootfs-template/. /mnt/target/
cp -a /bin/busybox /mnt/target/bin/busybox

echo "[4/6] copying kernel and boot initramfs"
mkdir -p /mnt/target/boot/extlinux
cp /usr/lib/syslinux/modules/bios/ldlinux.c32 /mnt/target/boot/extlinux/ 2>/dev/null || echo "  (no ldlinux.c32 in live fs)"
cp /boot/vmlinuz /mnt/target/boot/vmlinuz
cp /boot/initramfs-live.gz /mnt/target/boot/initramfs.gz

echo "[5/6] installing bootloader (extlinux)"
cat > /mnt/target/boot/extlinux/extlinux.conf <<CONF
DEFAULT nova-linux
TIMEOUT 30
LABEL nova-linux
  KERNEL /boot/vmlinuz
  INITRD /boot/initramfs.gz
  APPEND root=/dev/${TGT}1 console=ttyS0 console=tty0
CONF
ln -sf /proc/mounts /etc/mtab 2>/dev/null
extlinux --install /mnt/target/boot/extlinux 2>&1 || echo "  (extlinux install failed)"
dd if=/usr/lib/altmbr.bin of=/dev/$TGT bs=439 count=1 2>/dev/null
printf '\001' | dd of=/dev/$TGT bs=1 seek=439 conv=notrunc 2>/dev/null

echo "[6/6] syncing"
umount /mnt/target
sync
echo ""
echo "==============================================================="
echo "  install complete - you can now reboot into Nova-Linux"
echo "  from /dev/$TGT"
echo "==============================================================="
EOS
chmod +x $R/usr/sbin/novainstall

cat > $R/etc/motd <<'EOM'
Nova-Linux 1.0 (Sun) - NovaOS project
EOM

# kernel + boot initramfs inside the live system (used by the installer)
mkdir -p $R/boot
cp kpkg/boot/vmlinuz-6.8.0-31-generic $R/boot/vmlinuz
# rescue initramfs for the installed system: same rootfs but WITHOUT ./boot
# (no self-inclusion -> no runaway growth between builds)
( cd $R && find . -path ./boot -prune -o -print | cpio -o -H newc --quiet | gzip -9 ) > initramfs-live.gz

# pack main live initramfs
cd $R && find . | cpio -o -H newc --quiet | gzip -9 > /tmp/nlbuild/initramfs-new.gz && cd /tmp/nlbuild && mv initramfs-new.gz initramfs.cpio.gz
cp kpkg/boot/vmlinuz-6.8.0-31-generic vmlinuz && cp initramfs.cpio.gz initramfs.gz
du -sh initramfs.gz vmlinuz initramfs-live.gz

# ISO (isolinux, BIOS boot)
mkdir -p iso/boot/isolinux iso/nova
cp /usr/lib/ISOLINUX/isolinux.bin iso/boot/isolinux/
cp /usr/lib/syslinux/modules/bios/ldlinux.c32 iso/boot/isolinux/ 2>/dev/null || true
cat > iso/boot/isolinux/isolinux.cfg <<'CFG'
DEFAULT nova-linux-live
TIMEOUT 30
LABEL nova-linux-live
  KERNEL /boot/vmlinuz
  INITRD /boot/initramfs.gz
  APPEND console=ttyS0 console=tty0
CFG
cp vmlinuz iso/boot/
cp initramfs.gz iso/boot/
xorriso -as mkisofs -o nova-linux-1.0-sun.iso -b boot/isolinux/isolinux.bin \
  -c boot/isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table \
  -V NOVA_LINUX iso 2>/dev/null
ls -la nova-linux-1.0-sun.iso
echo "BUILD OK"
