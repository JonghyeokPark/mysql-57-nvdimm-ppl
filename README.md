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
$ ./build.sh --origin
```

## Run

1. Modify the following server variables to the `my.cnf` file:

| System Variable                     | Description | 
| :---------------------------------- | :---------- |
| innodb_use_ipl                      | Specifies whether to enable per-page logging scheme. **true** or **false**. |
| innodb_nvdimm_home_dir				      | NVDIMM-aware files resident directory |

2. Run MySQL server:

```bash
$ ./bld/bin/mysqld --defaults-file=my.cnf
```
