# Per-Page Logging on NVDIMM for MySQL 5.7

Boosting Transaction Performance using Per-Page Logging on NVDIMM

## Build and install

1. Clone the source code:

```bash
$ git clone https://github.com/JonghyeokPark/mysql-57-nvdimm-ppl
```

2. Pass your sudo password (`PASSWD`) as a first parameter of build script and run:

```bash
$ ./build.sh PASSWD
```

For vanilla version, you can run the script as follows:

```bash
$ ./build.sh PASSWD --origin
```

## Run MySQL Server

1. Modify the following server variables to the `my.cnf` file:

| System Variable                     | Description | 
| :---------------------------------- | :---------- |
| innodb_use_ipl                      | Specifies whether to enable per-page logging scheme. **true** or **false**. |
| innodb_nvdimm_home_dir				      | NVDIMM-aware files resident directory |
| innodb_nvdimm_size		              | The size in bytes of the NVDIMM. The default value is 1GB. |
| innodb_nvdimm_static_entry_size			| The size in bytes of each PPL block. The default value is 64B. |
| innodb_nvdimm_max_ppl_size				  | The size in bytes of the max PPL size, which can be allocated per page. The default value is 256B. |

For example:

```bash
$ vi my.cnf
...
innodb_use_ipl=true
innodb_nvdimm_home_dir=/mnt/pmem
innodb_nvdimm_size=1G
innodb_nvdimm_static_entry_size=64
innodb_nvdimm_max_ppl_size=256
...
```

2. Run MySQL server:

```bash
$ ./bld/bin/mysqld --defaults-fele=my.cnf
```
