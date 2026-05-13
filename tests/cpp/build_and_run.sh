#!/bin/bash

mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .

if [ $? -eq 0 ]; then
    ./jam_tests
else
    exit 1
fi