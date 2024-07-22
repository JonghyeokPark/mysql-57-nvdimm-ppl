#Linkbench Benchmark for Experiment

> This experiment guideline is cited from <https://github.com/facebookarchive/linkbench>

## Prerequisites:
--------------
These instructions assume you are using a UNIX-like system such as a Linux distribution
or Mac OS X.

**Java**: You will need a Java 7+ runtime environment.  LinkBench by default
      uses the version of Java on your path.  You can override this by setting the
      JAVA\_HOME environment variable to the directory of the desired
      Java runtime version.  You will also need a Java JDK to compile from source.

**Maven**: To build LinkBench, you will need the Apache Maven build tool. If
    you do not have it already, it is available from http://maven.apache.org .

**MySQL Connector**:  To benchmark MySQL with LinkBench, you need MySQL
    Connector/J, A version of the MySQL connector is bundled with
    LinkBench.  If you wish to use a more recent version, replace the
    mysql jar under lib/.  See http://dev.mysql.com/downloads/connector/j/

**MySQL Server**: To benchmark MySQL you will need a running MySQL
    server with free disk space.

## Getting and Building LinkBench
----------------------------
First get the source code

    git clone git@github.com:facebook/linkbench.git

Then enter the directory and build LinkBench

    cd linkbench
    mvn clean package

## MySQL Setup
-----------
We need to create a new database and tables on the MySQL server.
We'll create a new database called `linkdb` and
the needed tables to store graph nodes, links and link counts.
Run the following commands in the MySQL console:

    create database linkdb;
    use linkdb;

    CREATE TABLE `linktable` (
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


## Configuration Files
-------------------
Before benchmarking you will want to make a copy of the example config file:

    cp config/LinkConfigMysql.properties config/MyConfig.properties

Open MyConfig.properties.  At a minimum you will need to fill in the
settings under *MySQL Connection Information* to match the server, user
and database you set up earlier. E.g.

    # MySQL connection information
    host = localhost
    user = linkbench
    password = your_password
    port = 3306
    dbid = linkdb

Notice that MyConfig.properties references another file in this line:

    workload_file = config/FBWorkload.properties

The included workload file has been tuned to match our production workload in query mix.  
If you want to change the scale of the benchmark (the default graph is quite small for
benchmarking purposes), you should look at the maxid1 setting.  This
controls the number of nodes in the initial graph created in the load
phase: increase it to get a larger database.

      # start node id (inclusive)
      startid1 = 1

      # end node id for initial load (exclusive)
      # With default config and MySQL/InnoDB, 1M ids ~= 1GB
      # We set 50M ids ~= 64GB
      maxid1 = 50000001

## Loading Data
------------
First we need to do an initial load of data using our new config file:

    ./bin/linkbench -c config/MyConfig.properties -l

This will take a while to load, and you should get frequent progress updates.
At the end LinkBench reports a range of statistics on load time that are
of limited interest at this stage.

### Request Phase
-------------
Open MyConfig.properties. 
First, Set a Thread Number as same as CPU cores in your environment
Second, Set read + write requests per thread
Third, Set warmup_time to 100

    ...
    # Experiment Setting
    requesters = 32
    requests = 17500000
    maxtime = 10800
    warmup_time = 100
    ...

Run the request phase using the below command:

    ./bin/linkbench -c config/MyConfig.properties -r

LinkBench will log progress to the console, along with statistics.
Once all requests have been sent, or the time limit has elapsed, LinkBench
will notify you of completion:

    REQUEST PHASE COMPLETED. 25000000 requests done in 2266 seconds.
      Requests/second = 11029

You can also inspect the latency statistics. For example, the following line tells us the mean latency
for link range scan operations, along with latency ranges for median (p50), 99th percentile (p99) and
so on.

    GET_LINKS_LIST count = 12678653  p25 = [0.7,0.8]ms  p50 = [1,2]ms
                   p75 = [1,2]ms  p95 = [10,11]ms  p99 = [15,16]ms
                   max = 2064.476ms  mean = 2.427ms
