#!/usr/bin/env bash
set -e

mkdir -p build_debug
pushd build_debug
cmake .. -DCMAKE_BUILD_TYPE=Debug -DLTO=OFF -DLINKER_GC=OFF
make -j6
popd

ln -fs build_debug/rvmicro debug_rvmicro
ln -fs build_debug/rvnewlib debug_rvnewlib
ln -fs build_debug/rvlinux debug_rvlinux
