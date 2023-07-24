#!/bin/bash

# loading data
echo "Loading MySQL data start"
sudo rm -rf /mnt/pmem/nvdimm_mmap_file
sudo rm -rf /home/jhpark/test_data/*
sudo rm -rf /home/jhpark/test_log/*

# recovery test
cp -r /home/jhpark/backup/recovery-test/test_data/* /home/jhpark/test_data/
cp -r /home/jhpark/backup/recovery-test/test_log/* /home/jhpark/test_log/
cp -r /home/jhpark/backup/recovery-test/nvdimm_mmap_file /mnt/pmem/
rm -rf /home/jhpark/test_log/mysql_error_nvdimm.log

echo "Loading MySQL data finish, Recovery start!"

echo 'my.cnf (nvdimm) working!'
#sudo ./bld/bin/mysqld --defaults-file=./my-nvdimm.cnf --disable-log-bin &>/dev/null &disown
sudo gdb --args ./bld/bin/mysqld --defaults-file=./my-nvdimm.cnf --disable-log-bin
