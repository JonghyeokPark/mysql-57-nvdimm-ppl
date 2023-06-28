#!/bin/bash
PASSWD="vldb#7988"
result=$(ps -ef | grep /home/vldb/mysql-57-nvdimm-ipl/bld/bin/mysqld  | awk 'NR == 1 { print $2;}')
echo $PASSWD | sudo kill -9 $result