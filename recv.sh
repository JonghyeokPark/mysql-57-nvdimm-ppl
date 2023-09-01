#!/bin/bash

# loading data
echo "Loading MySQL data start"
sudo rm -rf /mnt/pmem/nvdimm_mmap_file
sudo rm -rf /home/jhpark/test_data/*
sudo rm -rf /home/jhpark/test_log/*

sudo cp -r /home/jhpark/backup/recovery-test/test_data/* /home/jhpark/test_data/
sudo cp -r /home/jhpark/backup/recovery-test/test_log/* /home/jhpark/test_log/
sudo cp /home/jhpark/backup/recovery-test/nvdimm_mmap_file /mnt/pmem/
rm -rf /home/jhpark/test_log/mysql_error_nvdimm.log

echo "recovery start!"

#echo 'my.cnf (nvdimm) working!'
gdb --args ./bld/bin/mysqld --defaults-file=./my-nvdimm.cnf --disable-log-bin #&>/dev/null &disown
#sudo ./bld/bin/mysqld --defaults-file=./my-nvdimm.cnf --disable-log-bin &>/dev/null &disown
