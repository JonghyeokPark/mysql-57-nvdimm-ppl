#!/bin/bash
# Load TPC-C data using the Vanilla build, end-to-end.
# Wipes /mnt/test_data + /mnt/test_log, initializes a fresh datadir,
# starts vanilla mysqld, creates the tpcc DB, loads $WH warehouses.
#
# Usage:
#   ./load_vanilla.sh             # default 100 warehouses
#   ./load_vanilla.sh 500         # 500 warehouses (~54 GB)

set -e

WH=${1:-100}

BASE_DIR=/home/vldb/mysql-57-nvdimm-ppl
VANILLA_DIR=$BASE_DIR/bld-vanilla
DATA=/mnt/test_data
LOG=/mnt/test_log
TPCC=$BASE_DIR/tpcc-mysql

fail() { echo "[!] $*" >&2; exit 1; }

# 0. Pre-flight
[ -d "$BASE_DIR" ]                  || fail "BASE_DIR not found: $BASE_DIR"
[ -x "$VANILLA_DIR/bin/mysqld" ]    || fail "Vanilla mysqld not found: $VANILLA_DIR/bin/mysqld (run ./build_vanilla.sh first)"
[ -f "$BASE_DIR/my-vanilla.cnf" ]   || fail "my-vanilla.cnf not found"
[ -d "$DATA" ]                      || fail "DATA dir not found (mount issue?): $DATA"
[ -d "$LOG" ]                       || fail "LOG dir not found (mount issue?): $LOG"
[ -d "$TPCC" ]                      || fail "tpcc-mysql dir not found: $TPCC"
[ -x "$TPCC/tpcc_load" ]            || fail "tpcc_load binary not found in $TPCC (build: cd tpcc-mysql/src && make)"
[ -f "$TPCC/create_table.sql" ]     || fail "create_table.sql missing"
[ -f "$TPCC/add_fkey_idx.sql" ]     || fail "add_fkey_idx.sql missing"

echo "[+] Pre-flight OK (warehouses=$WH)"

# 1. Stop any running mysqld (with 30s timeout, then SIGKILL)
if pgrep -x mysqld >/dev/null; then
    echo "[*] Stopping running mysqld..."
    "$VANILLA_DIR/bin/mysqladmin" -uroot -S /tmp/mysql.sock shutdown 2>/dev/null \
        || pkill -x mysqld || true
    for i in $(seq 1 30); do
        pgrep -x mysqld >/dev/null || break
        sleep 1
    done
    if pgrep -x mysqld >/dev/null; then
        echo "[!] mysqld did not exit in 30s, sending SIGKILL"
        pkill -9 -x mysqld || true
        sleep 2
    fi
    if pgrep -x mysqld >/dev/null; then
        echo "[!] mysqld still alive after SIGKILL — manual intervention needed"
        pgrep -af mysqld
        exit 1
    fi
    echo "[*] mysqld stopped"
fi

# 2. Wipe data + log dirs
echo "[*] Clearing $DATA"
rm -rf "$DATA"/* "$DATA"/.[!.]* 2>/dev/null || true
echo "[*] Clearing $LOG"
rm -rf "$LOG"/*  "$LOG"/.[!.]*  2>/dev/null || true

# 3. Initialize fresh datadir (insecure: empty root password, fine since
#    my-vanilla.cnf uses skip-grant-tables)
echo "[*] Initializing datadir at $DATA"
"$VANILLA_DIR/bin/mysqld" --initialize-insecure \
    --innodb_page_size=4k \
    --innodb_undo_tablespaces=2 \
    --user=root \
    --datadir="$DATA" \
    --basedir="$VANILLA_DIR" \
    --log-error="$LOG/mysql_init.log" \
    || { echo "[!] init failed:"; tail -30 "$LOG/mysql_init.log" 2>/dev/null; exit 1; }

# 4. Start vanilla server
echo "[*] Starting Vanilla mysqld"
cd "$BASE_DIR"
nohup "$VANILLA_DIR/bin/mysqld" --defaults-file="$BASE_DIR/my-vanilla.cnf" \
    >/tmp/mysqld_vanilla.log 2>&1 &
SERVER_PID=$!
echo "    PID=$SERVER_PID  startup_log=/tmp/mysqld_vanilla.log  error_log=$LOG/mysql_error_nvdimm.log"

# 5. Wait for socket
for i in $(seq 1 60); do
    [ -S /tmp/mysql.sock ] && break
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "[!] mysqld died early. Tail of error log:" >&2
        tail -30 "$LOG/mysql_error_nvdimm.log" 2>/dev/null
        exit 1
    fi
    sleep 1
done
[ -S /tmp/mysql.sock ] || fail "Timeout waiting for /tmp/mysql.sock"
echo "[+] Server is up"

MYSQL="$VANILLA_DIR/bin/mysql -uroot -S /tmp/mysql.sock"

# 6. Create database + schema (per README order: tables, then fkey/idx, then load)
echo "[*] Creating tpcc DB and tables"
$MYSQL -e "DROP DATABASE IF EXISTS tpcc; CREATE DATABASE tpcc;"
$MYSQL tpcc < "$TPCC/create_table.sql"
$MYSQL tpcc < "$TPCC/add_fkey_idx.sql"

# 7. Load data via tpcc-mysql/load.sh (parallel: 1 items job + 3 jobs per
#    100-warehouse stripe, all in background, then wait)
echo "[*] Loading $WH warehouses via tpcc-mysql/load.sh (parallel)"
export LD_LIBRARY_PATH="$VANILLA_DIR/lib"
cd "$TPCC"
rm -f 1.out 2_*.out 3_*.out 4_*.out 2>/dev/null
time bash ./load.sh tpcc $WH

# Sanity: confirm rows landed (tpcc_load prints usage + exits 0 on bad args)
LOADED_W=$($MYSQL -N -e "SELECT COUNT(*) FROM tpcc.warehouse" 2>/dev/null)
LOADED_S=$($MYSQL -N -e "SELECT COUNT(*) FROM tpcc.stock" 2>/dev/null)
echo "[*] post-load row counts: warehouse=$LOADED_W, stock=$LOADED_S"
if [ "${LOADED_W:-0}" -lt 1 ] || [ "${LOADED_S:-0}" -lt 1 ]; then
    fail "tpcc_load did not insert rows (warehouse=$LOADED_W, stock=$LOADED_S)"
fi

echo
echo "[+] Load complete: $WH warehouses."
echo "    Server still running. Shut down with:"
echo "      $VANILLA_DIR/bin/mysqladmin -uroot -S /tmp/mysql.sock shutdown"
echo "    Backup:"
echo "      rsync -a --no-owner --no-group $DATA/  /mnt/back_up/mysql_57_tpcc_${WH}w/test_data/"
echo "      rsync -a --no-owner --no-group $LOG/   /mnt/back_up/mysql_57_tpcc_${WH}w/test_log/"
