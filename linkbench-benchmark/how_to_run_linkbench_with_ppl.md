# Run Linkbench with NV-PPL

1. Pass your sudo password (PASSWD) as a first parameter of the build script and build with NV-PPL version:

```bash
$ cd mysql-57-nvdimm-ppl
$ sudo rm -rf bld
$ ./build.sh PASSWD --ppl
```

2. Open MyConfig.properties and modify the experiments variable

```bash
$ cd linkbench/config
```

```bash
# number of threads to run during request phase
requesters = 32  	#(32: # of cores in your machine)

# read + write requests per thread
requests = 781250 	# 32 * 781250 = 25M Requests

# warmup time in seconds.  The benchmark is run for a warmup period
warmup_time = 100
```

3. Modify the configuration file (`my.cnf` in repository) for your `data directory`, `log directory` and `persistent memory mount directory`
```bash
$ cd mysql-57-nvdimm-ppl
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


3. Start the NV-PPL server.

```bash
$ ./bld/bin/mysqld --defaults-file=my.cnf
```

5. Run the request phase:

```bash
$ cd linkbench
$ ./bin/linkbench -c config/MyConfig.properties -csvstats final-stats.csv -csvstream streaming-stats.csv -L linkbench_result.txt -r
```
