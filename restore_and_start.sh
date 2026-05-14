#!/bin/bash
# Restore a TPC-C backup to /mnt/test_data + /mnt/test_log, clear /mnt/pmem,
# then start mysqld with $CNF.
#
# Usage:
#   ./restore_and_start.sh          # default 10W
#   ./restore_and_start.sh 500      # use mysql_57_tpcc_500w backup
#   ./restore_and_start.sh 100      # use mysql_57_tpcc_100w backup

set -e

WH=${1:-10}
SUFFIX=${2:-}        # optional backup suffix: e.g. "undo" → mysql_57_tpcc_100w_undo

BASE_DIR=/home/vldb/mysql-57-nvdimm-ppl
BACKUP=/mnt/back_up/mysql_57_tpcc_${WH}w${SUFFIX:+_$SUFFIX}
DATA=/mnt/test_data
LOG=/mnt/test_log
PMEM=/mnt/pmem

# Always use HTAP cnf (PPL-MVCC ON, throttle settings)
CNF=my-htap.cnf
echo "[*] Using config: $CNF (warehouses=$WH)"

# 0. Pre-flight: every required directory and binary must exist
fail() { echo "[!] $*" >&2; exit 1; }

[ -d "$BASE_DIR" ]            || fail "BASE_DIR not found: $BASE_DIR"
[ -x "$BASE_DIR/bld/bin/mysqld" ]    || fail "mysqld binary not found: $BASE_DIR/bld/bin/mysqld"
[ -f "$BASE_DIR/$CNF" ]     || fail "$CNF not found: $BASE_DIR/$CNF"
[ -d "$BACKUP" ]              || fail "BACKUP not found: $BACKUP"
[ -d "$BACKUP/test_data" ]    || fail "BACKUP/test_data not found"
[ -d "$BACKUP/test_log" ]     || fail "BACKUP/test_log not found"
[ -f "$BACKUP/test_data/ibdata1" ] || fail "BACKUP looks empty: $BACKUP/test_data/ibdata1 missing"
[ -d "$DATA" ]                || fail "DATA dir not found (mount issue?): $DATA"
[ -d "$LOG" ]                 || fail "LOG dir not found (mount issue?): $LOG"
[ -d "$PMEM" ]                || fail "PMEM dir not found (DAX mount missing?): $PMEM"
mountpoint -q "$PMEM" 2>/dev/null || echo "[!] WARNING: $PMEM is not a mountpoint (NVDIMM/DAX not mounted?)"

echo "[+] Pre-flight OK"

# 0.4. Ensure data/log/pmem dirs are world-writable (777)
echo "[*] sudo chmod 777 on $DATA $LOG $PMEM"
sudo chmod 777 "$DATA" "$LOG" "$PMEM" || fail "chmod 777 failed"

# 0.5. Install build artifacts (requires sudo password)
echo "[*] Running 'sudo make -j install' in $BASE_DIR/bld ..."
( cd "$BASE_DIR/bld" && sudo make -j install ) || fail "make install failed"

# 1. Stop any running mysqld (with 30s timeout, then SIGKILL)
if pgrep -x mysqld >/dev/null; then
    echo "[*] mysqld is running; shutting down..."
    "$BASE_DIR/bld/bin/mysqladmin" -uroot -S /tmp/mysql.sock shutdown 2>/dev/null \
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

# 2. Clear destinations
echo "[*] Clearing $DATA"
rm -rf "$DATA"/* "$DATA"/.[!.]* 2>/dev/null || true
echo "[*] Clearing $LOG"
rm -rf "$LOG"/*  "$LOG"/.[!.]*  2>/dev/null || true
echo "[*] Clearing $PMEM (incl. nvdimm_mmap_file)"
rm -f "$PMEM/nvdimm_mmap_file" 2>/dev/null || true
rm -rf "$PMEM"/* "$PMEM"/.[!.]* 2>/dev/null || true

# 3. Copy backup
echo "[*] Copying backup data ($(du -sh "$BACKUP/test_data" | cut -f1))..."
rsync -a --no-owner --no-group --no-perms --omit-dir-times --info=progress2 "$BACKUP/test_data/" "$DATA/"
echo "[*] Copying backup logs ($(du -sh "$BACKUP/test_log" | cut -f1))..."
rsync -a --no-owner --no-group --no-perms --omit-dir-times --info=progress2 "$BACKUP/test_log/" "$LOG/"

# 4. Sanity check
if [ ! -f "$DATA/ibdata1" ]; then
    echo "[!] Restore failed: $DATA/ibdata1 missing" >&2
    exit 1
fi

# 5. Start server in background (with core dumps enabled)
echo "[*] Starting mysqld..."
cd "$BASE_DIR"
ulimit -c unlimited
nohup ./bld/bin/mysqld --defaults-file=$CNF >/tmp/mysqld_start.log 2>&1 &
SERVER_PID=$!
echo "[*] mysqld PID=$SERVER_PID, log=/tmp/mysqld_start.log, error_log=$LOG/mysql_error_nvdimm.log"

# 6. Wait for socket to appear
for i in $(seq 1 60); do
    if [ -S /tmp/mysql.sock ]; then
        echo "[+] Server is up after ${i}s"
        exit 0
    fi
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "[!] mysqld died; check $LOG/mysql_error_nvdimm.log" >&2
        tail -30 "$LOG/mysql_error_nvdimm.log" 2>/dev/null
        exit 1
    fi
    sleep 1
done
echo "[!] Timeout waiting for /tmp/mysql.sock" >&2
exit 1
