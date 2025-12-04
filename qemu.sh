
id=ubuntu-24.04.3-live-server-amd64.iso
iso="${id}.iso"
img="${id}.img.qcow2"

qemu-system-x86_64 -name guest=niklas-qemu-test,debug-threads=on \
			-machine q35,usb=off,dump-guest-core=off \
			-accel kvm -cpu host,migratable=on,hv-time=on,hv-relaxed=on,hv-vapic=on,hv-spinlocks=0x1fff  \
			-smp 4,sockets=2,cores=2,threads=1 -boot strict=on -kernel arch/x86_64/boot/bzImage \
			-initrd /home/john/buildroot/output/images/rootfs.cpio -nographic  \
			-append 'mode:1024x768 numa=fake=4 console=ttyS0 null_blk.bs=1024' -m 3G  \
			-device piix3-ide,id=ide \
			-drive id=disk,file=/home/john/ide.img,format=raw,if=none -device ide-hd,drive=disk,bus=ide.1 \
			\
			-L /home/john/mnt_sda4/john/qemu-1/ \

# -append "null_blk.bs=2048"
		
# nbd-server 9998 /home/john/mnt_sda4/john/mkp-scsi/Win2k.img.gcow2
# nbd-client 192.168.68.109 9998 -b 8192 /dev/nbd0
# nbd-client 192.168.68.109 9998 -b 4096 /dev/nbd0

# more /sys/block/nbd0/queue/logical_block_size
# more /sys/block/nullb0/queue/logical_block_size


#dd if=/dev/zero of=file.img bs=1M count=200
#losetup -b 2048 /dev/loop0 file.img
#losetup -b 8192 /dev/loop0 file.img
#more /sys/block/loop0/queue/logical_block_size

