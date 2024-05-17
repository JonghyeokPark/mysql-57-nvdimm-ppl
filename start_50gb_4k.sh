#!/bin/bash

user_input=$1
test_data_path=/test_data_960
test_log_path=/log_data
nvdimm_file_path=/pmem
bld_path=/home/vldb/mysql-57-nvdimm-ipl/bld
# tpcc_back_up_path=/back_up_data/tpcc_data_for_ppl
tpcc_back_up_path=/back_up_data/new_tpcc_data_50_4k
# tpcc_back_up_path=/back_up_data/new_tpcc_data_10_4k
# tpcc_back_up_path=/back_up_data/tpcc_data_pmm_4k
link_bench_path=/back_up_data/linkbench_50_4k
script_folder=/home/vldb/shell_script



red_header="\e[31m"
red_footer="\e[0m"
passwd="vldb#7988"

check_disk_space() {
    echo $passwd | sudo -S df -h > /dev/null
}

cd $bld_path
echo $passwd | sudo -S make -j install

check_disk_space
cd $script_folder
# check_disk_space
# bash dd_all_ssd.sh nvme /dev/nvme $test_data_path
check_disk_space
bash blk_discard.sh nvme /dev/nvme2n1 $test_data_path
bash blk_discard.sh sata /dev/sdb $test_log_path

echo -e $red_header"Start removing all file in test_data_path, test_log_path"$red_footer
cd $test_data_path && rm -rf *
cd $test_log_path && rm -rf *
cd $nvdimm_file_path && rm -rf *
echo -e $red_header"End Removing all file in test_data_path, test_log_path"$red_footer

if [ "$1" = "tpcc" ]; then
    echo -e $red_header"Start copying data from tpcc_back_up_path to test_data_path"$red_footer
    cd $tpcc_back_up_path && cp -r  * $test_data_path
    echo -e $red_header"End copying data from tpcc_back_up_path to test_data_path"$red_footer
elif [ "$1" = "linkbench" ]; then
    echo -e $red_header"Start copying data from link_bench_data to test_data_path"$red_footer
    cd $link_bench_path && cp -r * $test_data_path
    echo -e $red_header"End copying data from link_bench_data to test_data_path"$red_footer
else
    echo -e $red_header"Start copying data from tpcc_back_up_path to test_data_path"$red_footer
    cd $tpcc_back_up_path && cp -r  * $test_data_path
    echo -e $red_header"End copying data from tpcc_back_up_path to test_data_path"$red_footer
fi
check_disk_space
echo $passwd | sudo -S sysctl vm.drop_caches=3
#cd $bld_path && valgrind --leak-check=yes ./bin/mysqld --defaults-file=/home/vldb/mysql-57-nvdimm-ipl/my_4k.cnf &
cd $bld_path && ./bin/mysqld --defaults-file=/home/vldb/mysql-57-nvdimm-ipl/my_4k_11G.cnf &
#cd $bld_path && gdb --args ./bin/mysqld --defaults-file=/home/vldb/mysql-57-nvdimm-ipl/my_4k_11G.cnf --disable-log-bin #&>/dev/null &disown 
# check_disk_space
echo $passwd | sudo -S sysctl vm.drop_caches=3

if [ "$1" = "linkbench" ]; then
linkbench_folder=/home/vldb/linkbench
sleep 30
cd $linkbench_folder && bash benchmark.sh $2 $bld_path
elif [ "$1" = "tpcc" ]; then
tpcc_folder=/home/vldb/tpcc-mysql
sleep 50
# cd $tpcc_folder && bash benchmark.sh $2 $bld_path
cd $tpcc_folder && bash execute_tpcc.sh
fi

