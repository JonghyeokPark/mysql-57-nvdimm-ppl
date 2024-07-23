## How to install tpcc-mysql

### Prerequisite

- libmysqlclient-dev

```bash
$ sudo apt-get install libmysqlclient-dev
```

### Installation

1. Clone *tpcc-mysql* from the [Percona GitHub repository](https://github.com/Percona-Lab/tpcc-mysql):

```bash
$ git clone https://github.com/Percona-Lab/tpcc-mysql.git
```

2. Go to the tpcc-mysql directory and build binaries:

```bash
$ cd tpcc-mysql/src
$ make
```

3. Before running the benchmark, you should create a database for the TPC-C test. Go to the MySQL base directory and run the following commands:

```bash
$ cd mysql-57-nvdimm-ppl
$ ./bld/bin/mysql -u root -p -e "CREATE DATABASE tpcc;"
$ ./bld/bin/mysql -u root -p tpcc < /path/to/tpcc-mysql/create_table.sql
$ ./bld/bin/mysql -u root -p tpcc < /path/to/tpcc-mysql/add_fkey_idx.sql
```

4. Then go back to the tpcc-mysql directory and load data:

> Change the warehouse value (`-w 500`). One warehouse occupies about 100MB (500 warehouses ~= 54GB).

```bash
$ cd tpcc-mysql
$ ./tpcc_load -h 127.0.0.1 -d tpcc -u root -p "yourPassword" -w 500
*************************************
*** TPCC-mysql Data Loader        ***
*************************************
option h with value '127.0.0.1'
option d with value 'tpcc'
option u with value 'root'
option p with value 'yourPassword'
option w with value '500'
<Parameters>
     [server]: 127.0.0.1
     [port]: 3306
     [DBname]: tpcc
	 [user]: root
     [pass]: yourPassword
	 [warehouse]: 500
TPCC Data Load Started...
Loading Item
.................................................. 5000
.................................................. 10000
.................................................. 15000
...
...DATA LOADING COMPLETED SUCCESSFULLY.
```

5. After Loading the data is finished, shut down the MySQL server:

```bash
$ cd mysql-57-nvdimm-ppl
$ ./bld/bin/mysqladmin -uroot -p shutdown
```

## Reference
- https://github.com/meeeejin/til
- https://github.com/LeeBohyun/mysql-tpcc
- [Build and install the source code (5.7)](https://github.com/meeeejin/til/blob/master/mysql/build-and-install-the-source-code-5.7.md)
- [Percona-Lab/tpcc-mysql](https://github.com/Percona-Lab/tpcc-mysql)
- [tpcc-mysql: Simple usage steps and how to build graphs with gnuplot](https://www.percona.com/blog/2013/07/01/tpcc-mysql-simple-usage-steps-and-how-to-build-graphs-with-gnuplot/)
- [MySQL 5.7 Server System Variable Reference](https://dev.mysql.com/doc/refman/5.7/en/server-system-variable-reference.html)