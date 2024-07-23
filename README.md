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

## Run MySQL Server

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
| innodb_use_ppl_mvcc	              | Specifies whether to make page version with PPL in NVDIMM. **true** or **false**.|

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

## Run Benchmark

This repository includes experiment guidelines for the benchmarks(TPC-C, Linkbench) with NV-PPL
* TPC-C: under `tpcc-benchmark` directory
* Linkbench: under `linkbench-benchmark` directory
