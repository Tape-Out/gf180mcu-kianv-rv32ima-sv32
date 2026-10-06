#!/usr/bin/env bash
# 把测试台与给定的 Verilog 编成 <目录>/Vtb。顶层必须叫 tb，口子是 clk、rst_n、pad_in、pad_out、pad_oe 三条 54 位总线。
# 用法：sim.sh <目录> <verilator 的源文件与选项…>
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
D=$(realpath -m "$1")
shift
mkdir -p "$D"
verilator --cc --exe --build -j 0 -O3 --savable --x-assign fast --x-initial fast --noassert \
  -Wno-fatal -Wno-lint -Wno-style -Wno-TIMESCALEMOD -Wno-BLKANDNBLK --top-module tb --Mdir "$D" -o Vtb \
  -CFLAGS "-O2 -std=c++17" "$@" "$H/tb.cpp" > "$D/build.log" 2>&1 || { tail -n 40 "$D/build.log"; exit 1; }
