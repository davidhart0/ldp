#!/bin/sh
set -e
set -x
mkdir -p build
cd build
cmake ..
make
./src/test_ldp

