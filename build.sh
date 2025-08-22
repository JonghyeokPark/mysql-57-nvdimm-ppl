#!/bin/bash

# if [ $# -ne 2];
# then
# 	echo "usage: ./build.sh PASSWORD --ppl"
# 	exit 1
# fi

BASE_DIR=`pwd -P`
BUILD_DIR=$BASE_DIR/bld
PASSWD=$1

# Clean and make a directory for build
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning existing build directory"
    echo $PASSWD | sudo -S rm -rf $BUILD_DIR/*
else
    echo "Make a directory for build"
    mkdir bld
fi

cd $BUILD_DIR

# Build and install the source code
if [ "$2" = "--origin" ]; then
  # vanilla
  BUILD_FLAGS=""
elif [ "$2" = "--ppl" ]; then
  # Enable PPL
  BUILD_FLAGS="-DUNIV_NVDIMM_PPL"
else
  BUILD_FLAGS="-DUNIV_NVDIMM_PPL"
fi

echo "Start build using $BUILD_FLAGS"

cd $BUILD_DIR

cmake .. -DWITH_DEBUG=0 -DCMAKE_C_FLAGS="$BUILD_FLAGS" -DCMAKE_CXX_FLAGS="$BUILD_FLAGS" \
-DDOWNLOAD_BOOST=ON -DWITH_BOOST=$BASE_DIR/boost -DENABLED_LOCAL_INFILE=1 \
-DCMAKE_INSTALL_PREFIX=$BUILD_DIR

make -j
echo $PASSWD | sudo -S make install
