#!/usr/bin/env bash
# 标准 riscv-tests 的 p 环境，在整颗 SoC 上跑：程序直接放进 SDRAM 模型，Flash 里只有两条指令跳过去，测试台盯 tohost。
# 用法：isa.sh <输出目录> [<旋钮>=<值> …]
# SIM=<目录> 时用那里编好的 Vtb（芯片仓拿交付的 .v 编的那一份），不自己编，旋钮也就不看了
set -euo pipefail
cd "$(dirname "$0")/.."
O=$(realpath -m "$1")
shift
X=${CROSS:-riscv64-unknown-elf-}
R=$PWD/third_party/riscv-tests
[ -f "$R/env/p/link.ld" ] || { echo "riscv-tests 的子模块没取：git submodule update --init --recursive"; exit 1; }
mkdir -p "$O"
if [ -z "${SIM:-}" ]; then
  bash htest/setup.sh
  # shellcheck disable=SC2046
  bash htest/sim.sh "$O/sim" htest/tb_rtl.v $(python3 htest/args.py "$@")
  SIM=$O/sim
fi
# 复位向量在 Flash 里：lui t0, 0x80000; jr t0
printf '\267\002\000\200\147\200\002\000' > "$O/jump.bin"

# 编一个、跑一个，退出码即结果；第二个参数是环境的头文件目录
one() {
  local t=$1 env=$2 e=$3
  # -fno-pic：Linux 工具链默认位置无关，env 里的 la 会出 GOT 重定位
  "${X}gcc" -march=rv32ima_zicsr_zifencei -mabi=ilp32 -static -mcmodel=medany -fvisibility=hidden \
    -nostdlib -nostartfiles -fno-pic -no-pie -I "$env" -I "$R/env/p" -I "$R/isa/macros/scalar" \
    -T "$R/env/p/link.ld" "$t" -o "$e.elf"
  "${X}objcopy" -O binary "$e.elf" "$e.bin"
  "$SIM/Vtb" +flash="$O/jump.bin@0x100000" +sdram="$e.bin@0" \
    +tohost="0x$("${X}nm" "$e.elf" | awk '$3 == "tohost" {print $1}')" +max=6000000 > "$e.log" 2>&1
}

# 不适用的不跑：Zacas 不是 A 本身；非对齐访存、PMP、调试触发器是可选的，KianV 都没做
SKIP=" rv32ua-p-amocas_w rv32ua-p-amocas_d rv32ui-p-ma_data rv32mi-p-pmpaddr rv32mi-p-breakpoint "
# 已知失败，必须恰好失败，哪天过了也算红，好知道该改这里（原因见 README）
KNOWN=" rv32mi-p-illegal rv32mi-p-instret_overflow rv32mi-p-ma_addr rv32mi-p-ma_fetch rv32mi-p-shamt
        rv32si-p-dirty rv32si-p-ma_fetch "

n=0; bad=0; kept=0
for s in rv32ui rv32um rv32ua rv32mi rv32si; do
  for t in "$R"/isa/$s/*.S; do
    b=$s-p-$(basename "$t" .S)
    [[ $SKIP == *" $b "* ]] && continue
    e="$O/$b"
    n=$((n + 1))
    if one "$t" "$R/env/p" "$e"; then
      [[ $KNOWN == *" $b"[[:space:]]* ]] && { echo "FAIL $b 是已知失败却过了"; bad=$((bad + 1)); }
    elif [[ $KNOWN == *" $b"[[:space:]]* ]]; then
      kept=$((kept + 1))
    else
      echo "FAIL $b：$(grep -a 'tohost' "$e.log" | tail -n 1)"; bad=$((bad + 1))
    fi
  done
done

echo "跑 $n 个，$bad 个不过，已知失败 $kept 个，不适用 $(wc -w <<< "$SKIP") 个"
[ "$n" -gt 0 ] && [ "$bad" = 0 ]
