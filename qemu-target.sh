qemu-system-x86_64 -name guest=qemu-target-test,debug-threads=on \
			-machine q35,usb=off,dump-guest-core=off \
			-accel kvm -cpu host,migratable=on,hv-time=on,hv-relaxed=on,hv-vapic=on,hv-spinlocks=0x1fff  \
			-smp 4,sockets=2,cores=2,threads=1 -boot strict=on -kernel arch/x86_64/boot/bzImage \
			-initrd /home/john/mnt_sda4/john/buildroot2/output/images/rootfs.cpio -nographic  \
			-append 'mode:1024x768 numa=fake=4 console=ttyS0 null_blk.bs=1024' -m 3G  \
			\
			-L /home/john/mnt_sda4/john/qemu-1/ \
			\
			-netdev bridge,id=user1,br=br0 \
			-device virtio-net-pci,netdev=user1,mac=52:54:33:41:63:01 \
			-netdev bridge,id=user0,br=br0 \
			-device virtio-net-pci,netdev=user0,mac=52:54:34:42:62:01 \
			
		#	-append 'mode:1024x768 numa=fake=4 console=ttyS0 null_blk.bs=1024' \
		#	-kernel arch/x86_64/boot/bzImage \
		#	-initrd /home/john/mnt_sda4/john/buildroot2/output/images/rootfs.cpio \
		
		
		# old nvme for native mpath
		#	-L /home/john/mnt_sda4/john/qemu-1/ \
		#	-device nvme-subsys,id=nvme-subsys-0,nqn=subsys0 \
		#	-device nvme,serial=deadbeef,subsys=nvme-subsys-0 \
		#	-device nvme,serial=deadbeef,subsys=nvme-subsys-0 \
		#	-drive file=/home/john/mnt_sda4/john/nvme-rootfs.img,if=none,id=mynvme0  \
		#	-device nvme-ns,drive=mynvme0,shared=on,nsid=1 \
		#	-netdev bridge,id=user1,br=br0 \

