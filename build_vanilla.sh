#!/bin/bash
# Build a Vanilla MySQL (no NV-PPL) into ./bld-vanilla/, alongside the
# PPL build in ./bld/. Pass sudo password as $1 if you want `make install`,
# otherwise the binaries are left in bld-vanilla/ uninstalled.

set -e

BASE_DIR=`pwd -P`
BUILD_DIR=$BASE_DIR/bld-vanilla
PASSWD=$1

# Pre-flight
[ -f "$BASE_DIR/CMakeLists.txt" ] || { echo "[!] Run from repo root"; exit 1; }
[ -d "$BASE_DIR/boost" ] || echo "[*] boost/ not found; cmake will download (-DDOWNLOAD_BOOST=ON)"

# Make / clean build dir
if [ -d "$BUILD_DIR" ]; then
    echo "[*] Cleaning existing $BUILD_DIR"
    if [ -n "$PASSWD" ]; then
        echo "$PASSWD" | sudo -S rm -rf "$BUILD_DIR"/*
    else
        rm -rf "$BUILD_DIR"/* 2>/dev/null || sudo rm -rf "$BUILD_DIR"/*
    fi
else
    echo "[*] Creating $BUILD_DIR"
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Vanilla = no UNIV_NVDIMM_PPL flag
echo "[*] Configuring (Vanilla, no PPL)"
cmake .. -DWITH_DEBUG=0 \
    -DCMAKE_C_FLAGS="" -DCMAKE_CXX_FLAGS="" \
    -DDOWNLOAD_BOOST=ON -DWITH_BOOST=$BASE_DIR/boost \
    -DENABLED_LOCAL_INFILE=1 \
    -DCMAKE_INSTALL_PREFIX=$BUILD_DIR

echo "[*] Building"
make -j

if [ -n "$PASSWD" ]; then
    echo "[*] Installing"
    echo "$PASSWD" | sudo -S make -j install
else
    echo "[*] Skipping make install (no password passed). To install:"
    echo "    cd $BUILD_DIR && sudo make -j install"
fi

echo "[+] Vanilla build done: $BUILD_DIR/bin/mysqld"
