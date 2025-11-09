#!/bin/bash
make CROSS_COMPILE=$(pwd)/gcc-compiler/bin/aarch64-none-linux-gnu- \
O=out ARCH=arm64 -j$(($(nproc)+1)) $@
