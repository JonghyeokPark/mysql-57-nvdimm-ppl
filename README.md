# Boosting Transaction Performance using Per-Page Logging on NVDIMM

* NV-PPL is a novel database architecture that leverages NVDIMM as a durable log cache so as to avoid the durability overhead in flash-based databases.
* NV-PPL captures per-page redo logs and stores them on NVDIMM, thus avoiding a significant fraction of page writes to the storage and boosting the performance of OLTP workloads dramatically.
* NV-PPL is implemented on MySQL 5.7/InnoDB with moderate code changes.

## Build and install

1. Clone the source code:

```bash
$ git clone https://github.com/JonghyeokPark/mysql-57-nvdimm-ppl
```

2. Pass your sudo password (`PASSWD`) as a first parameter of the build script and run:

```bash
$ ./build.sh PASSWD
```
The above command will compile and build the source code with the default option (i.e., per-page-logging on NVDIMM). The available options are:

| Option     | Description |
| :--------- | :---------- |
| --origin   | No per-page-logging (Vanilla version)  |
| --ppl      | Per-Page-Logging on NVDIMM (default)  |

For the vanilla version, you can run the script as follows:

```bash
$ ./build.sh PASSWD --origin
```

## Run NV-PPL with configuration options

1. Modify the following server variables to the `my.cnf` file:

| System Variable                     | Description | 
| :---------------------------------- | :---------- |
| innodb_use_nvdimm_ppl               | Specifies whether to enable per-page logging scheme. **true** or **false**. |
| innodb_nvdimm_home_dir              | NVDIMM-aware files resident directory |
| innodb_nvdimm_size		          | The size in bytes of the NVDIMM. The default value is 1GB. |
| innodb_nvdimm_ppl_block_size	  	  | The size in bytes of each PPL block. The default value is 64B. |
| innodb_nvdimm_max_ppl_size	      | The size in bytes of the max PPL size, which can be allocated per page. The default value is 256B. |
| innodb_use_nvdimm_redo	          | Specifies whether to place redo log buffer on NVDIMM. **true** or **false**.|
| innodb_use_nvdimm_dwb	              | Specifies whether to place the double write buffer on NVDIMM. **true** or **false**.|
| innodb_use_nvdimm_ppl_recovery	  | Specifies whether to enable per-page logging recovery. **true** or **false**.|
| innodb_use_ppl_cleaner	          | Specifies whether to enable PPL cleaner on NVDIMM. **true** or **false**.|
| innodb_use_ppl_mvcc	              | Specifies whether to enable PPL-based multi-version. **true** or **false**.|

For example:

```bash
$ vi my.cnf
...
innodb_use_nvdimm_ppl=true
innodb_nvdimm_home_dir=/mnt/pmem
innodb_nvdimm_size=1G
innodb_nvdimm_ppl_block_size=64
innodb_nvdimm_max_ppl_size=256
innodb_use_nvdimm_ppl_recovery=false
innodb_use_nvdimm_redo=true
innodb_use_nvdimm_dwb=true
innodb_use_ppl_cleaner=false
innodb_use_ppl_mvcc=false
...
```

2. Run MySQL server:

```bash
$ ./bld/bin/mysqld --defaults-fele=my.cnf
```

# How to test NV-PPL with TPC-C Benchamrk
From this section, we will describe how to test NV-PPL using the TPC-C benchmark. 

## How to install MySQL 5.7 for loading TPC-C Data
> Note: Before testing the NV-PPL with the TPC-C Benchmark, loading the TPC-C data with Vanilla MySQL is needed to run the benchmark.
### Prerequisites

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
----
### Build and install

1. build Vanilla MySQL with `--origin` option:
  
```bash
# * Pass your sudo password (`PASSWD`) as a first parameter of the build script
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

3. Modify the configuration file (`my-vanilla.cnf` in repository) for your `datadir` and `logdir`

```bash
$ vi my-vanilla.cnf
default-storage-engine = innodb
skip-grant-tables
pid-file        = /path/to/datadir/mysql.pid
socket          = /tmp/mysql.sock
port            = 3306
datadir         = /path/to/datadir/
log-error		= /path/to/logdir/mysql_error_nvdimm.log
innodb_log_group_home_dir = /path/to/logdir/
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

## How to install tpcc-mysql and load the data
> Note: tpcc-mysql from the [Percona GitHub repository](https://github.com/Percona-Lab/tpcc-mysql) is an implementation of the TPC-C benchmark specifically designed to work with MySQL databases.
### Prerequisite

- libmysqlclient-dev

```bash
$ sudo apt-get install libmysqlclient-dev
```
---
### Installation

Go to the tpcc-mysql directory and build binaries:

```bash
$ cd tpcc-mysql/src
$ make
```

---
### Loading Data
1. Before running the benchmark, you should create a database for the TPC-C test. Go to the MySQL base directory and run the following commands:

```bash
$ cd mysql-57-nvdimm-ppl
$ ./bld/bin/mysql -u root -p -e "CREATE DATABASE tpcc;"
$ ./bld/bin/mysql -u root -p tpcc < /path/to/tpcc-mysql/create_table.sql
$ ./bld/bin/mysql -u root -p tpcc < /path/to/tpcc-mysql/add_fkey_idx.sql
```

2. Go back to the tpcc-mysql directory and load data (500 warehouses ~= 54GB):

```bash
$ cd tpcc-mysql
$ ./tpcc_load -h 127.0.0.1 -d tpcc -u root -p "yourPassword" -w 500
```

3. After Loading the data is finished, shut down the MySQL server:

```bash
$ cd mysql-57-nvdimm-ppl
$ ./bld/bin/mysqladmin -uroot -p shutdown
```
---
## Testing NV-PPL performance with TPC-C benchmark

1. Pass your sudo password (PASSWD) as a first parameter of the build script and build with NV-PPL version:

```bash
$ cd mysql-57-nvdimm-ppl
$ sudo rm -rf bld
$ ./build.sh PASSWD --ppl
```

2. Modify the configuration file (`my.cnf` in repository) for your `datadir`, `logdir` and `persistent memory mount directory`
```bash
$ vi my.cnf
...
default-storage-engine = innodb
skip-grant-tables
pid-file        = /path/to/datadir/mysql.pid
socket          = /tmp/mysql.sock
port            = 3306
datadir         = /path/to/datadir/
log-error		= /path/to/logdir/mysql_error_nvdimm.log
innodb_log_group_home_dir=/path/to/logdir/
innodb_nvdimm_home_dir=/mnt/pmem
...
```


3. Start the MySQL server with NV-PPL.

```bash
$ ./bld/bin/mysqld --defaults-file=my.cnf
```

4. Run the TPC-C test:

```bash
$ cd tpcc-mysql
$ ./tpcc_start -h 127.0.0.1 -S /tmp/mysql.sock -d tpcc -u root -p "yourPassword" -w 500 -c 32 -r 300 -l 1800 -i 1 | tee tpcc-result.txt
```

# Testing other performances
For testing the other performances, experiment guidelines are below:
* [Testing NV-PPL with the Linkbench benchmark](https://github.com/JonghyeokPark/mysql-57-nvdimm-ppl/blob/paper_version/how_to_test_with_linkbench.md)
* [Testing NV-PPL recovery performance](https://github.com/JonghyeokPark/mysql-57-nvdimm-ppl/blob/paper_version/how_to_test_recovery.md)
* Testing NV-PPL HTAP performance(?)

# Plotting graph scripts
> Note: Before plotting the graph, run the experiment first. Then, execute the script with the following parameter:

After executing the scripts, check the **`plots`** directory to see the graphs
## Prerequisite

- gnuplot

```bash
$ sudo apt-get install gnuplot
```

## Plotting TPS graph for TPC-C
- `tpcc-result-path`: The absolute path to the `tpcc-result.txt` file

```bash
$ python3 ./plots/plot_tpcc_tps_graph.py /tpcc-result-path
```

## Plotting OPS graph for Linkbench

- `linkbench-result-path`: The absolute path to the `linkbench-result.txt` file

```bash
$ python3 ./plots/plot_linkbench_ops_graph.py /linkbench-result-path
```

## Plotting recovery time graph

- `logdir`: The absolute path to the MySQL log directory

```bash
$ python3 ./plots/plot_recovery_graph.py /logdir/mysql_error_nvdimm.log
```



# Reference
- https://github.com/meeeejin/til
- [Build and install the source code (5.7)](https://github.com/meeeejin/til/blob/master/mysql/build-and-install-the-source-code-5.7.md)
- [Percona-Lab/tpcc-mysql](https://github.com/Percona-Lab/tpcc-mysql)
- [tpcc-mysql: Simple usage steps and how to build graphs with gnuplot](https://www.percona.com/blog/2013/07/01/tpcc-mysql-simple-usage-steps-and-how-to-build-graphs-with-gnuplot/)
- [MySQL 5.7 Server System Variable Reference](https://dev.mysql.com/doc/refman/5.7/en/server-system-variable-reference.html)
