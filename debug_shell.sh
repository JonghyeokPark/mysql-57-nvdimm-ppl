#!/bin/bash

result=$(ps -ef | grep home/vldb/mysql-57-nvdimm-ipl/bld/bin/mysqld  | awk 'NR == 1 { print $2;}')
sudo gdb -p $result