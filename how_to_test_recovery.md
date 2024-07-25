# How to test NV-PPL recovery performance

1. Pass your sudo password (PASSWD) as a first parameter of the build script and build with NV-PPL version:

```bash
$ ./build.sh PASSWD --ppl
```

2. Start the MySQL server with NV-PPL:

```bash
$ ./bld/bin/mysqld --defaults-file=my.cnf
```

3. Run the TPC-C benchmark:

```bash
$ cd tpcc-mysql
$ ./tpcc_start -h 127.0.0.1 -S /tmp/mysql.sock -d tpcc -u root -p "yourPassword" -w 500 -c 32 -r 0 -l 1800 -i 1 | tee tpcc-result.txt
```

4. After **`10 minutes`**, shutdown the MySQL server with `SIGKILL`:
```bash
$ ps -ef | grep mysqld # Check the MySQL server pid
$ sudo pkill -9 -ef mysqld
```
5. Change the value of `innodb_use_nvdimm_ppl_recovery` to `true` in the `my.cnf` file:
```bash
$ vi my.cnf
...
innodb_use_nvdimm_ppl_recovery=true
...
```
6. Restart the MySQL server with NV-PPL:
```bash
$ ./bld/bin/mysqld --defaults-file=my.cnf
```

7. Check the recovery performance for NV-PPL in each phase (`Analysis`, `Redo`, `Undo`):
	- `scan_time`: Time spent in the `Analysis` phase
	- `redo_time`: Time spent in the `Redo` phase
	- `undo_time`: Time spent in the `Undo` phase
```bash
$ grep -E "scan_time|redo_time|undo_time" /path/to/logdir/mysql_error_nvdimm.log

#Result
scan_time: 57.071975 seconds
redo_time: 27.989681 seconds
undo_time: 0.131537 seconds
```
