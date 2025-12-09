cp  /home/john/buildroot/output/images/rootfs.cpio /home/john/buildroot/output/images/rootfs_ini.cpio

qemu-system-x86_64 -name guest=qemu-ini-test,debug-threads=on \
			-smp cpus=4 \
			-object memory-backend-ram,size=3G,id=m0 \
			-numa node,memdev=m0,cpus=0,nodeid=0 \
			-numa node,cpus=1,nodeid=1 \
			-numa node,cpus=2,nodeid=2 \
			-numa node,cpus=3,nodeid=3 \
			-machine q35,usb=off,dump-guest-core=off \
			-accel kvm -cpu host,migratable=on,hv-time=on,hv-relaxed=on,hv-vapic=on,hv-spinlocks=0x1fff  \
			-smp 4,sockets=2,cores=2,threads=1 -boot strict=on -kernel arch/x86_64/boot/bzImage \
			-initrd /home/john/buildroot/output/images/rootfs_ini.cpio -nographic  \
			-append 'mode:1024x768 numa=fake=4 console=ttyS0 null_blk.bs=1024 scsi_multipath.enable=1' -m 3G  \
			\
			-L /home/john/mnt_sda4/john/qemu-1/ \
    			-netdev bridge,id=user0,br=br0 \
       			-device virtio-net-pci,netdev=user0,mac=56:78:00:00:00:00 \
    			-netdev bridge,id=user1,br=br0 \
       			-device virtio-net-pci,netdev=user1,mac=56:78:01:01:01:01 \
    			
    			
    			
    			#-net bridge,br=br0                       \
       			#-net nic,model=virtio,macaddr=52:54:00:00:00:01 \
       			
       			#-initrd /home/john/mnt2/john/buildroot2/output/images/rootfs.cpio -nographic  \
       			
       			
