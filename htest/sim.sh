#!/usr/bin/env bash
# 把测试台与给定的 Verilog 编成 <目录>/Vtb。顶层必须叫 tb，口子是 clk、rst_n、pad_in、pad_out、pad_oe 三条 54 位总线。
# 用法：sim.sh <目录> <verilator 的源文件与选项…>
# 给了 SIM_GPU=<to2610_gpu.v> 时把那颗也编进去（前缀 Vgpu，单独成库再链进来），测试台的 +gpu 才能用
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
D=$(realpath -m "$1")
shift
mkdir -p "$D"
extra=()
if [ -n "${SIM_GPU:-}" ]; then
  verilator --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style -Wno-TIMESCALEMOD \
    -Wno-BLKANDNBLK --top-module to2610_gpu --prefix Vgpu --Mdir "$D/gpu" "$SIM_GPU" > "$D/gpu.log" 2>&1 \
    || { tail -n 40 "$D/gpu.log"; exit 1; }
  make -s -C "$D/gpu" -f Vgpu.mk Vgpu__ALL.a >> "$D/gpu.log" 2>&1 || { tail -n 40 "$D/gpu.log"; exit 1; }
  extra=(-CFLAGS "-DWITH_GPU -I$D/gpu" "$D/gpu/Vgpu__ALL.a")
fi
verilator --cc --exe --build -j 0 -O3 --savable --x-assign fast --x-initial fast --noassert \
  -Wno-fatal -Wno-lint -Wno-style -Wno-TIMESCALEMOD -Wno-BLKANDNBLK --top-module tb --Mdir "$D" -o Vtb \
  -CFLAGS "-O2 -std=c++17" "$@" "${extra[@]}" "$H/tb.cpp" > "$D/build.log" 2>&1 || { tail -n 40 "$D/build.log"; exit 1; }
