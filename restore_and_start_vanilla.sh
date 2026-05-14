#!/bin/bash
# Restore TPC-C backup to /mnt/test_data + /mnt/test_log, start Vanilla MySQL
# (no PPL) using my-vanilla.cnf. Mirror of restore_and_start.sh for the
# vanilla baseline.
#
# Usage:
#   ./restore_and_start_vanilla.sh         # default 10W
#   ./restore_and_start_vanilla.sh 500     # use mysql_57_tpcc_500w backup

set -e

WH=${1:-10}
SUFFIX=${2:-}        # optional backup suffix: e.g. "undo" → mysql_57_tpcc_100w_undo

BASE_DIR=/home/vldb/mysql-57-nvdimm-ppl
VANILLA_DIR=$BASE_DIR/bld-vanilla
BACKUP=/mnt/back_up/mysql_57_tpcc_${WH}w${SUFFIX:+_$SUFFIX}
DATA=/mnt/test_data
LOG=/mnt/test_log
# Always use vanilla-htap.cnf (HTAP settings: small buffer + purge throttle)
CNF=my-vanilla-htap.cnf

echo "[*] Using config: $CNF (warehouses=$WH, vanilla)"

fail() { echo "[!] $*" >&2; exit 1; }

[ -d "$BASE_DIR" ]                 || fail "BASE_DIR not found: $BASE_DIR"
[ -x "$VANILLA_DIR/bin/mysqld" ]   || fail "Vanilla mysqld missing: $VANILLA_DIR/bin/mysqld"
[ -f "$BASE_DIR/$CNF" ]            || fail "$CNF not found in $BASE_DIR"
[ -d "$BACKUP" ]                   || fail "BACKUP not found: $BACKUP"
[ -f "$BACKUP/test_data/ibdata1" ] || fail "BACKUP looks empty: $BACKUP/test_data/ibdata1"
[ -d "$DATA" ]                     || fail "DATA dir missing: $DATA"
[ -d "$LOG" ]                      || fail "LOG dir missing: $LOG"

echo "[+] Pre-flight OK"

echo "[*] sudo chmod 777 on $DATA $LOG"
sudo chmod 777 "$DATA" "$LOG" || fail "chmod 777 failed"

echo "[*] Running 'sudo make -j install' in $VANILLA_DIR ..."
( cd "$VANILLA_DIR" && sudo make -j install ) || fail "make install failed"

if pgrep -x mysqld >/dev/null; then
    echo "[*] mysqld is running; shutting down..."
    "$VANILLA_DIR/bin/mysqladmin" -uroot -S /tmp/mysql.sock shutdown 2>/dev/null \
        || pkill -x mysqld || true
    for i in $(seq 1 30); do
        pgrep -x mysqld >/dev/null || break
        sleep 1
    done
    if pgrep -x mysqld >/dev/null; then
        echo "[!] SIGKILL"
        pkill -9 -x mysqld || true
        sleep 2
    fi
    pgrep -x mysqld >/dev/null && fail "mysqld still alive"
    echo "[*] mysqld stopped"
fi

echo "[*] Clearing $DATA"
rm -rf "$DATA"/* "$DATA"/.[!.]* 2>/dev/null || true
echo "[*] Clearing $LOG"
rm -rf "$LOG"/*  "$LOG"/.[!.]*  2>/dev/null || true

echo "[*] Restoring data ($(du -sh "$BACKUP/test_data" | cut -f1))..."
rsync -a --no-owner --no-group --no-perms --omit-dir-times --info=progress2 "$BACKUP/test_data/" "$DATA/"
echo "[*] Restoring logs ($(du -sh "$BACKUP/test_log" | cut -f1))..."
rsync -a --no-owner --no-group --no-perms --omit-dir-times --info=progress2 "$BACKUP/test_log/" "$LOG/"

[ -f "$DATA/ibdata1" ] || fail "Restore failed: $DATA/ibdata1 missing"

echo "[*] Starting Vanilla mysqld..."
cd "$BASE_DIR"
ulimit -c unlimited
nohup "$VANILLA_DIR/bin/mysqld" --defaults-file="$BASE_DIR/$CNF" >/tmp/mysqld_vanilla.log 2>&1 &
SERVER_PID=$!
echo "[*] PID=$SERVER_PID  log=/tmp/mysqld_vanilla.log  error_log=$LOG/mysql_error_nvdimm.log"

for i in $(seq 1 60); do
    [ -S /tmp/mysql.sock ] && { echo "[+] Server up after ${i}s"; exit 0; }
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "[!] mysqld died"
        tail -30 "$LOG/mysql_error_nvdimm.log" 2>/dev/null
        exit 1
    fi
    sleep 1
done
fail "Timeout waiting for /tmp/mysql.sock"
