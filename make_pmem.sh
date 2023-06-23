#!/bin/bash

pmem_dir=/dev/pmem0
pmem_mount_dir=/dram_simulate

sudo mkfs.ext4 $pmem_dir
sudo mount -o dax $pmem_dir $pmem_mount_dir
sudo chmod 777 $pmem_mount_dir