
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-1.img
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-2.img
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-3.img
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-4.img
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-5.img
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-6.img
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-7.img
truncate -s 0 /home/john/mnt_sda4/john/nvme-rootfs-8.img

fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-1.img
fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-2.img
fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-3.img
fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-4.img
fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-5.img
fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-6.img
fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-7.img
fallocate -l 400M /home/john/mnt_sda4/john/nvme-rootfs-8.img
