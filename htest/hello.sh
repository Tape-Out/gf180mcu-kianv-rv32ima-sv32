#!/usr/bin/env bash
# 上游源码配测试台：复位后从 Flash 的 0x2010_0000 起一段裸机程序，设好串口分频，读写 SDRAM，从串口报结果。
# 用法：hello.sh <输出目录> [<旋钮>=<值> …]，旋钮不给就取默认值
set -euo pipefail
cd "$(dirname "$0")/.."
O=$(realpath -m "$1")
shift
rm -rf "$O"
mkdir -p "$O"
clk=50000000
for a in "$@"; do [ "${a%%=*}" = systemClk ] && clk=${a#*=}; done
# shellcheck disable=SC2046
bash htest/sim.sh "$O/sim" htest/tb_rtl.v $(python3 htest/args.py "$@")
make -s -C htest/hello O="$O/hello"
"$O/sim/Vtb" +flash="$O/hello/hello.bin@0x100000" +script=htest/hello/script +uartdiv=$((clk / 115200))   +max=30000000 | tee "$O/uart.log"
