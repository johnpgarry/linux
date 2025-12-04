#!/usr/bin/env bash
set -eu
# Tested on host: Ubuntu 18.10.
# TODO: get working without GUI:
# https://askubuntu.com/questions/1108334/how-to-boot-and-install-the-ubuntu-server-image-on-qemu-nographic-without-the-g
id=ubuntu-24.04.3-live-server-amd64.iso
iso="${id}.iso"
img="${id}-ini.img.qcow2"
#if [ ! -f "$iso" ]; then
#  wget "http://releases.ubuntu.com/18.04/${iso}"
#fi
if [ ! -f "$img" ]; then
  qemu-img create -f qcow2 "$img" 1T
fi
#qemu-system-x86_64 \
#  -cdrom /home/john/mnt_sda4/john/ubuntu-24.04.3-live-server-amd64.iso \
#  -enable-kvm \
#  -m 2G \
#  
#  -smp 2 \
# 
  
export DISPLAY=:0
  
qemu-system-x86_64 -name guest=niklas-qemu-test,debug-threads=on \
			-cdrom /home/john/mnt_sda4/john/ubuntu-24.04.3-live-server-amd64.iso \
			-drive "file=${img},format=qcow2" \
			-machine q35,usb=off,dump-guest-core=off \
			-accel kvm -cpu host,migratable=on,hv-time=on,hv-relaxed=on,hv-vapic=on,hv-spinlocks=0x1fff  \
			-smp 4,sockets=2,cores=2,threads=1 -boot strict=on \
			-serial mon:stdio \
			-m 3G  \
			-device piix3-ide,id=ide \
			-drive id=disk,file=/home/john/mnt_sda4/john/ide.img,format=raw,if=none -device ide-hd,drive=disk,bus=ide.1 \
			\
			-L /home/john/mnt_sda4/john/qemu-1/ \
			-netdev bridge,id=user1,br=br0 \
			-device virtio-net-pci,netdev=user1,mac=52:12:00:00:63:01 \
			-netdev bridge,id=user0,br=br0 \
			-device virtio-net-pci,netdev=user0,mac=52:34:00:00:62:01 \
			
		#	-append 'mode:1024x768 numa=fake=4 console=ttyS0 null_blk.bs=1024' \
		#	-kernel arch/x86_64/boot/bzImage \
		#	-initrd /home/john/mnt_sda4/john/buildroot2/output/images/rootfs.cpio \

