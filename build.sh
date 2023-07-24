#!/bin/bash

if [ $# -ne 2 ] 
then
	echo "usage: ./build.sh PASSWORD --ipl"
	exit 1;
fi

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
  # No caching
  BUILD_FLAGS=""
elif [ "$2" = "--ipl" ]; then
  # Enable Flash-Friendly Checkpoint
  BUILD_FLAGS="-DUNIV_NVDIMM_IPL -DUNIV_IPL_DEBUG"
else
  BUILD_FLAGS="-DUNIV_NVDIMM_IPL"
fi

echo "Start build using $BUILD_FLAGS"

cd $BUILD_DIR

cmake .. -DWITH_DEBUG=0 -DCMAKE_C_FLAGS="$BUILD_FLAGS" -DCMAKE_CXX_FLAGS="$BUILD_FLAGS" \
-DDOWNLOAD_BOOST=ON -DWITH_BOOST=$BASE_DIR/boost -DENABLED_LOCAL_INFILE=1 \
-DCMAKE_INSTALL_PREFIX=$BUILD_DIR

make -j
echo $PASSWD | sudo -S make install
