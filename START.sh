#!/bin/bash

# loading data
echo "Loading MySQL data start"
sudo rm -rf /mnt/pmem/nvdimm_mmap_file
sudo rm -rf /home/jhpark/test_data/*
sudo rm -rf /home/jhpark/test_log/*

# tpcc 10 wh
#cp -r /home/jhpark/backup/tpcc-10/* /home/jhpark/test_data/
# tpcc 500 wh
cp -r /home/jhpark/backup/tpcc-500-16KB/test_data/* /home/jhpark/test_data/
echo "Loading MySQL data finish"

#echo 'my.cnf (nvdimm) working!'
#gdb --args ./mysqld --defaults-file=../../my.cnf --disable-log-bin #&>/dev/null &disown
sudo ./bld/bin/mysqld --defaults-file=./my-nvdimm.cnf --disable-log-bin &>/dev/null &disown
