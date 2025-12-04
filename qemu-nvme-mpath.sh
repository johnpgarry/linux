
id=ubuntu-24.04.3-live-server-amd64.iso
iso="${id}.iso"
img="${id}.img.qcow2"

qemu-system-x86_64 -name guest=niklas-qemu-test,debug-threads=on \
			-machine q35,usb=off,dump-guest-core=off \
			-accel kvm -cpu host,migratable=on,hv-time=on,hv-relaxed=on,hv-vapic=on,hv-spinlocks=0x1fff  \
			-smp 4,sockets=2,cores=2,threads=1 -boot strict=on -kernel arch/x86_64/boot/bzImage \
			-initrd /home/john/mnt_sda4/john/buildroot2/output/images/rootfs.cpio -nographic  \
			-append 'mode:1024x768 numa=fake=4 console=ttyS0 null_blk.bs=1024' -m 3G  \
			-device piix3-ide,id=ide \
			-drive id=disk,file=/home/john/mnt_sda4/john/ide.img,format=raw,if=none -device ide-hd,drive=disk,bus=ide.1 \
			\
			-L /home/john/mnt_sda4/john/qemu-1/ \
			-device virtio-net-pci,netdev=user0,mac=52:54:00:00:62:01 \
			-netdev bridge,id=user0,br=br0 \
			-device virtio-net-pci,netdev=user1,mac=52:54:00:00:63:01 \
			-netdev bridge,id=user1,br=br0 \
			-device nvme-subsys,id=nvme-subsys-0,nqn=subsys0 \
			-device nvme,serial=deadbeef,subsys=nvme-subsys-0 \
			-device nvme,serial=deadbeef,subsys=nvme-subsys-0 \
			-drive file=/home/john/mnt_sda4/john/nvme-rootfs.img,if=none,id=mynvme0  \
			-device nvme-ns,drive=mynvme0,shared=on,nsid=1 \
			-drive "file=${img},format=qcow2" \

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

