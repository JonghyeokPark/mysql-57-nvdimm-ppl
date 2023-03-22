#!/bin/bash

mount_disk=/dev/sdb1
mount_dir=/test_data_micron

sudo mkfs.ext4 $mount_disk
sudo mount -o dax $mount_disk $mount_dir
sudo chmod 777 $mount_dir
