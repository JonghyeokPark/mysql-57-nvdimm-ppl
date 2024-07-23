# Run Linkbench with NV-PPL

1. Pass your sudo password (PASSWD) as a first parameter of the build script and build with NV-PPL version:

```bash
$ cd mysql-57-nvdimm-ppl
$ sudo rm -rf bld
$ ./build.sh PASSWD --ppl
```

2. Modify the configuration file (`my.cnf` in repository) for your `data directory`, `log directory` and `persistent memory mount directory`
```bash
$ vi my.cnf

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
log-error	= /path/to/logdir/mysql_error_nvdimm.log

#Log group path (iblog0, iblog1)
innodb_log_group_home_dir=/path/to/logdir/

# NVDIMM-IPL settings
innodb_nvdimm_home_dir=/mnt/pmem
...
```


3. Start the MySQL server with NV-PPL version.

```bash
$ ./bld/bin/mysqld --defaults-file=my.cnf
```

4. Run the TPC-C test:

```bash
$ ./tpcc_start -h 127.0.0.1 -S /tmp/mysql.sock -d tpcc -u root -p "yourPassword" -w 500 -c 32 -r 300 -l 1800 -i 1 | tee tpcc-result.txt
```

It means:

- Host: 127.0.0.1
- MySQL Socket: /tmp/mysql.sock
- DB: tpcc
- User: root
- Password: yourPassword
- Warehouse: 500
- Connection: 32
- Rampup time: 300 (sec)
- Measure: 1800 (sec)
- Result interval: 1 (sec)

### Output

```bash
***************************************
*** ###easy### TPC-C Load Generator ***
***************************************
option h with value '127.0.0.1'
option S (socket) with value '/tmp/mysql.sock'
option d with value 'tpcc'
option u with value 'root'
option p with value 'yourPassword'
option w with value '500'
option c with value '32'
option r with value '300'
option l with value '1800'
<Parameters>
     [server]: 127.0.0.1
     [port]: 3306
     [DBname]: tpcc
       [user]: root
       [pass]: yourPassword
  [warehouse]: 500
 [connection]: 32
     [rampup]: 300 (sec.)
    [measure]: 1800 (sec.)

RAMP-UP TIME.(300 sec.)

MEASURING START.

1, trx: 1365, 95%: 18.365, 99%: 21.568, max_rt: 27.011, 1360|11.575, 137|6.899, 137|50.943, 137|89.240
2, trx: 1343, 95%: 18.777, 99%: 24.195, max_rt: 29.087, 1342|10.322, 134|9.024, 131|58.391, 134|108.671
3, trx: 1323, 95%: 19.003, 99%: 22.538, max_rt: 27.470, 1327|8.282, 132|14.367, 134|46.692, 134|105.742
...
1800, trx: 1215, 95%: 20.989, 99%: 24.797, max_rt: 36.546, 1214|23.508, 122|13.674, 120|64.434, 122|96.620
STOPPING THREADS........
...
<TpmC>
                 73417.133 TpmC
```
Note the `TpmC` value. The metric for evaluating TPC-C performance is the number of transactions completed per minute (TpmC).

## Reference
* https://github.com/meeeejin/til
* https://github.com/LeeBohyun/mysql-tpcc