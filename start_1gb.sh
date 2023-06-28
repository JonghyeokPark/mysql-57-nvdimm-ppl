#!/bin/bash

test_data_path=/test_data_micron
test_log_path=/home/vldb/mysql-57-nvdimm-ipl/test_log
nvdimm_file_path=/dram_simulate
bld_path=/home/vldb/mysql-57-nvdimm-ipl/bld
first_setting=/home/vldb/16kb_10ware

# second_setting=/home/vldb/8kb_10ware
# third_setting=/test_data

cd $test_data_path && rm -rf *
cd $first_setting && cp -r  * $test_data_path
cd $test_log_path && rm -f *
cd $nvdimm_file_path && rm -rf * 
cd $bld_path && ./bin/mysqld_safe --defaults-file=/home/vldb/mysql-57-nvdimm-ipl/my_1gb.cnf