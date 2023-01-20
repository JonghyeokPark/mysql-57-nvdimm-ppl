bld_path=/back_up/mysql-57-nvdimm-ipl/bld
tpcc_path=/back_up/tpcc-mysql

cd $bld_path
./bin/mysql -u root -e "CREATE DATABASE tpcc;"
./bin/mysql -u root tpcc < $tpcc_path/create_table.sql
./bin/mysql -u root tpcc < $tpcc_path/add_fkey_idx.sql

cd $tpcc_path
./tpcc_load -h 127.0.0.1 -d tpcc -u root -w 10