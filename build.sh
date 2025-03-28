#!/bin/bash

# if [ $# -ne 2];
# then
# 	echo "usage: ./build.sh PASSWORD --ppl"
# 	exit 1
# fi

BASE_DIR=`pwd -P`
BUILD_DIR=$BASE_DIR/bld
PASSWD=$1

# Make a directory for build
if [ ! -d "$BUILD_DIR" ]; then
    echo "Make a directory for build"
    mkdir bld
fi

cd $BUILD_DIR

rm -rf CMakeCache.txt
echo $PASSWD | sudo -S rm -rf CMakeFiles/*

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

make -j8
echo $PASSWD | sudo -S make install
