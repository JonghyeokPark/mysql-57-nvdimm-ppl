# How to test NV-PPL with Linkbench benchmark 
> Note: Before testing the NV-PPL with the Linkbench Benchmark, loading the Linkbench data with Vanilla MySQL is needed to run the benchmark.

## How to install LinkBench and load the data

### Prerequisites

- Java

1. Install the latest [JDK](https://www.oracle.com/java/technologies/javase-jdk13-downloads.html).

2. Move and decompress the downloaded file:

```bash
$ cd /usr/local
$ sudo mkdir java
$ cd java
$ sudo mv jdk-13.0.2_linux-x64_bin.tar.gz .
$ sudo tar -zxvf jdk-13.0.2_linux-x64_bin.tar.gz
```

3. (Optional) Register the java executables:

```bash
$ sudo update-alternatives --install /usr/bin/java java /usr/local/java/jdk-13.0.2/bin/java 1
$ sudo update-alternatives --install /usr/bin/javac javac /usr/local/java/jdk-13.0.2/bin/javac 1
$ sudo update-alternatives --install /usr/bin/javaws javaws /usr/local/java/jdk-13.0.2/bin/javaws 1
```

If another version of Java is already installed, select the version of Java to use using below commands:

```bash
$ sudo update-alternatives --config java
$ sudo update-alternatives --config javac
$ sudo update-alternatives --config javaws
```

4. Add the three lines below (for setting the environment variables) to `/etc/profile` file:

```bash
$ sudo vi /etc/profile
...
export JAVA_HOME=/usr/local/java/jdk-13.0.2
export JRE_HOME=$JAVA_HOME/jre
export PATH=$PATH:$JAVA_HOME/bin:$JRE_HOME/bin

$ . /etc/profile
```

5. Check the version of Java:

```bash
$ java -version
java version "13.0.2" 2020-01-14
Java(TM) SE Runtime Environment (build 13.0.2+8)
Java HotSpot(TM) 64-Bit Server VM (build 13.0.2+8, mixed mode, sharing)
```

- Maven

```bash
$ sudo apt-get install maven
```

---
### Installation

1. Clone the source code:

```bash
$ git clone https://github.com/facebookarchive/linkbench.git
```

2. Build LinkBench (skip all tests):

```bash
$ cd linkbench
$ mvn clean package -DskipTests
```

3. Start the MySQL server .
> NOTE: If MySQL server is already on, skip this step.

```bash
$ cd mysql-57-nvdimm-ppl
$ ./bld/bin/mysqld --defaults-file=my-vanilla.cnf
```

4. Create a new database called `linkdb` and the needed tables:

```bash
$ ./bld/bin/mysql -uroot -pxxxx

mysql> create database linkdb;

mysql> use linkdb;

mysql> CREATE TABLE `linktable` (
  `id1` bigint(20) unsigned NOT NULL DEFAULT '0',
  `id2` bigint(20) unsigned NOT NULL DEFAULT '0',
  `link_type` bigint(20) unsigned NOT NULL DEFAULT '0',
  `visibility` tinyint(3) NOT NULL DEFAULT '0',
  `data` varchar(255) NOT NULL DEFAULT '',
  `time` bigint(20) unsigned NOT NULL DEFAULT '0',
  `version` int(11) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (link_type, `id1`,`id2`),
  KEY `id1_type` (`id1`,`link_type`,`visibility`,`time`,`id2`,`version`,`data`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1 PARTITION BY key(id1) PARTITIONS 16;

CREATE TABLE `counttable` (
  `id` bigint(20) unsigned NOT NULL DEFAULT '0',
  `link_type` bigint(20) unsigned NOT NULL DEFAULT '0',
  `count` int(10) unsigned NOT NULL DEFAULT '0',
  `time` bigint(20) unsigned NOT NULL DEFAULT '0',
  `version` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`id`,`link_type`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

CREATE TABLE `nodetable` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `type` int(10) unsigned NOT NULL,
  `version` bigint(20) unsigned NOT NULL,
  `time` int(10) unsigned NOT NULL,
  `data` mediumtext NOT NULL,
  PRIMARY KEY(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

mysql>quit;
```

5. Make a copy of the example config file:

```bash
$ cd linkbench
$ cp config/LinkConfigMysql.properties config/MyConfig.properties
```

6. Open `MyConfig.properties` and change the MySQL connection information. For example:

```bash
# MySQL connection information
host = localhost
user = root
password = xxxx
port = 3306
dbid = linkdb
```

If you want to change the scale of the benchmark, open `FBWorkload.properties` and increase the value of `maxid1` to get a larger database:

```bash
  # start node id (inclusive)
  startid1 = 1

  # end node id for initial load (exclusive)
  # With default config and MySQL/InnoDB, 50M ids ~= 64GB
  maxid1 = 50000001
```

7. Load the data:

```bash
$ ./bin/linkbench -c config/MyConfig.properties -l
```

8. After Loading the data is finished, shut down the MySQL server:

```bash
$ cd mysql-57-nvdimm-ppl
$ ./bld/bin/mysqladmin -uroot -p shutdown
```

## Testing NV-PPL performance with Linkbench benchmark

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

default-storage-engine = innodb
skip-grant-tables
pid-file        = /path/to/datadir/mysql.pid
socket          = /tmp/mysql.sock
port            = 3306
datadir         = /path/to/datadir/
log-error	= /path/to/logdir/mysql_error_nvdimm.log
innodb_log_group_home_dir=/path/to/logdir/
innodb_nvdimm_home_dir=/mnt/pmem
...
```

4. Start the NV-PPL server.

```bash
$ ./bld/bin/mysqld --defaults-file=my.cnf
```

5. Run the request phase:

```bash
$ cd linkbench
$ ./bin/linkbench -c config/MyConfig.properties -csvstats final-stats.csv -csvstream streaming-stats.csv -L linkbench-result.txt -r
```


## REFERENCE
* https://github.com/meeeejin/til
* https://github.com/facebookarchive/linkbench