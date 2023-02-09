#!/bin/bash

test_data_path=/test_data
test_log_path=/home/vldb/mysql-57-nvdimm-ipl/test_log
nvdimm_file_path=/dram_simulate
bld_path=/home/vldb/mysql-57-nvdimm-ipl/bld
test_data_sample_path=/home/vldb/mysql-57-nvdimm-ipl/test_data_sample

cd $test_data_path && rm -rf *
cd $test_data_sample_path && cp -r  * $test_data_path
cd $test_log_path && rm -f *
cd $nvdimm_file_path && rm -f * 
# cd $bld_path && ./bin/mysqld_safe --defaults-file=/home/vldb/mysql-57-nvdimm-ipl/my.cnf