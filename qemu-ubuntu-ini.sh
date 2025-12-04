#!/usr/bin/env bash
set -eu
# Tested on host: Ubuntu 18.10.
# TODO: get working without GUI:
# https://askubuntu.com/questions/1108334/how-to-boot-and-install-the-ubuntu-server-image-on-qemu-nographic-without-the-g
id=ubuntu-24.04.3-live-server-amd64-ini.iso
iso="${id}.iso"
img="${id}.img.qcow2"

#qemu-system-x86_64 \
#  -cdrom /home/john/mnt_sda4/john/ubuntu-24.04.3-live-server-amd64.iso \
#  -enable-kvm \
#  -m 2G \
#  
#  -smp 2 \
# 
  
export DISPLAY=:0
  
qemu-system-x86_64 -name guest=qemu-ini-test,debug-threads=on \
			-machine q35,usb=on,dump-guest-core=off \
			-accel kvm -cpu host,migratable=on,hv-time=on,hv-relaxed=on,hv-vapic=on,hv-spinlocks=0x1fff  \
			-smp 4,sockets=2,cores=2,threads=1 -boot strict=on \
			-serial mon:stdio \
			-m 6G  \
			-drive "file=/home/john/ubuntu_cow/${img},format=qcow2" \
			\
			-device usb-tablet \
			-m 3G  \
			-L /home/john/mnt_sda4/john/qemu-1/ \
			-netdev bridge,id=user1,br=br0 \
			-device virtio-net-pci,netdev=user1,mac=52:54:00:00:63:01 \
			-netdev bridge,id=user0,br=br0 \
			-device virtio-net-pci,netdev=user0,mac=52:54:00:00:62:01 \
			
		#	-append 'mode:1024x768 numa=fake=4 console=ttyS0 null_blk.bs=1024' \
		#	-kernel arch/x86_64/boot/bzImage \
		#	-initrd /home/john/mnt_sda4/john/buildroot2/output/images/rootfs.cpio \

