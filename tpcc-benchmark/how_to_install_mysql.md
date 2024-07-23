# How to install MySQL 5.7

Building MySQL 5.7 from the source code enables you to load the TPC-C data

## Prerequisites

- libreadline

```bash
$ sudo apt-get install libreadline6 libreadline6-dev
```

- libaio

```bash
$ sudo apt-get install libaio1 libaio-dev
$ sudo apt install libssl-dev
```

- etc.

```bash
$ sudo apt-get install build-essential cmake libncurses5 libncurses5-dev bison
```

## Build and install

1. build Vanilla MySQL (Using Vanilla MySQL for loading data):
  
```bash
# * Pass your sudo password (`PASSWD`) as a first parameter of the build script
$ git clone https://github.com/JonghyeokPark/mysql-57-nvdimm-ppl
$ cd mysql-57-nvdimm-ppl
$ ./build.sh PASSWD --origin
```

2. MySQL initialization:
`mysqld --initialize` handles initialization tasks that must be performed before the MySQL server, mysqld, is ready to use.
`datadir` and `logdir` must be empty for initialization.
	* `--datadir` : the path to the MySQL data directory
	* `--basedir` : the path to the MySQL installation directory
	* `--logdir`  : the path to the MySQL log directory

```bash
$ ./bld/bin/mysqld --initialize --innodb_page_size=4k --user=mysql --datadir=/path/to/datadir --basedir=/path/to/basedir
```

3. Modify the configuration file (`my-vanilla.cnf` in repository) for your `--datadir` and `--logdir`

```bash
$ vi my-vanilla.cnf

#
# The MySQL database server configuration file.
#
[mysqld]
user=root
#
# * Basic Settings
#
default-storage-engine = innodb
skip-grant-tables
pid-file        = /path/to/datadir/mysql.pid
socket          = /tmp/mysql.sock
port            = 3306
datadir         = /path/to/datadir/
log-error		= /path/to/logdir/mysql_error_nvdimm.log

#Log group path (iblog0, iblog1)
innodb_log_group_home_dir=/path/to/logdir/
```


4. Set the MySQL root password:

```bash
$ ./bld/bin/mysqld --defaults-file=./my-vanilla.cnf --skip-grant-tables
$ ./bld/bin/mysql -uroot

root:(none)> use mysql;
root:mysql> update user set authentication_string=password('yourPassword') where user='root';
root:mysql> flush privileges;
root:mysql> quit;

$ ./bld/bin/mysql -uroot -p

root:mysql> set password = password('yourPassword');
root:mysql> quit;
```

5. Open `.bashrc` and add MySQL to your path by below lines:

```bash
$ vi ~/.bashrc

export PATH=/path/to/basedir/bin:$PATH
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/basedir/lib/

$ source ~/.bashrc
```

6. Shut down and restart the MySQL server:

```bash
$ ./bld/bin/mysqladmin -uroot -p shutdown
$ ./bld/bin/mysqld --defaults-file=./my-vanilla.cnf 
```

## REFERENCE
* https://github.com/meeeejin/til
* https://github.com/LeeBohyun/mysql-tpcc