sudo mkfs.ext4 -F /dev/pmem0
sudo mkdir -p /mnt/pmem
sudo mount -o dax /dev/pmem0 /mnt/pmem
mount | grep pmem