#!/usr/bin/env bash
# 上游源码配测试台，两段裸机程序都从 Flash 的 0x2010_0000 起：
#   hello   设好串口分频，读写 SDRAM，从串口报结果
#   periph  GPIO、两路 SPI（测试台上各挂一个回声从设备）、计时器中断、软件中断、PLIC、重启
# 用法：hello.sh <输出目录> [<旋钮>=<值> …]，旋钮不给就取默认值
set -euo pipefail
cd "$(dirname "$0")/.."
O=$(realpath -m "$1")
shift
rm -rf "$O"
mkdir -p "$O"
bash htest/setup.sh
clk=50000000
for a in "$@"; do [ "${a%%=*}" = systemClk ] && clk=${a#*=}; done
# shellcheck disable=SC2046
bash htest/sim.sh "$O/sim" htest/tb_rtl.v $(python3 htest/args.py "$@")
make -s -C htest/hello O="$O/hello"
"$O/sim/Vtb" +flash="$O/hello/hello.bin@0x100000" +script=htest/hello/script +uartdiv=$((clk / 115200))   +max=30000000 | tee "$O/uart.log"
make -s -C htest/periph O="$O/periph"
"$O/sim/Vtb" +flash="$O/periph/periph.bin@0x100000" +script=htest/periph/script +spiecho +uartdiv=$((clk / 115200)) \
  +max=30000000 | tee "$O/periph.log"
