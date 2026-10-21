#!/bin/bash
# Copyright (C) 2024 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0

# This script is used to fuzz the sdcshield binary using AFL.
# Additional parameters will be passed directly to the meson setup command.
# In particular, you may want to use -Dafl_inc=/path/to/afl/include to specify
# custom location of AFL headers.

# Usage:
#./build-fuzzing.sh target_dir [meson setup options]

# Example:
# ./build-fuzzing.sh build-fuzzing -Dafl_inc=/path/to/afl/include

# Exit if afl-gcc is not found
if ! command -v afl-gcc &> /dev/null; then
    echo "afl-gcc not found. Please install AFL."
    exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

target_dir=$1
shift

CC=afl-gcc CXX=afl-g++ meson setup $target_dir -Dfuzzing=true $@
# 统一走受限并行度入口（默认 -j = nproc/2; 显式 -j/SDC_BUILD_JOBS 可覆盖）
CC=afl-gcc CXX=afl-g++ "$script_dir/build.sh" -C "$target_dir"
