#!/usr/bin/env bash
# riscv-arch-test（ACT4）的非特权部分在整颗 SoC 上跑：自检 ELF 放进 SDRAM 模型，Flash 里两条指令跳过去，测试台盯 tohost。
# ELF 由 htest/arch-test/build.sh 编好，包钉在 htest/arch-test.pin（地址与 sha256）；ACT_ELFS 给目录就用本机编的那一份。
# 用法：arch-test.sh <输出目录> [<旋钮>=<值> …]；SIM 的意思同 isa.sh
set -euo pipefail
cd "$(dirname "$0")/.."
O=$(realpath -m "$1")
shift
X=${CROSS:-riscv64-unknown-elf-}
mkdir -p "$O"
if [ -z "${SIM:-}" ]; then
  bash htest/setup.sh
  # shellcheck disable=SC2046
  bash htest/sim.sh "$O/sim" htest/tb_rtl.v $(python3 htest/args.py "$@")
  SIM=$O/sim
fi
E=${ACT_ELFS:-}
if [ -z "$E" ]; then
  read -r url sum < htest/arch-test.pin
  curl -fsSL --retry 5 -o "$O/elfs.tar.gz" "$url"
  echo "$sum  $O/elfs.tar.gz" | sha256sum -c - > /dev/null || { echo "ELF 包的摘要与 arch-test.pin 对不上"; exit 1; }
  E=$O/elfs
  rm -rf "$E" && mkdir -p "$E" && tar -xzf "$O/elfs.tar.gz" -C "$E"
fi
# 复位向量在 Flash 里：lui t0, 0x80000; jr t0
printf '\267\002\000\200\147\200\002\000' > "$O/jump.bin"

# 已知失败，必须恰好失败，哪天过了也算红；原因写在 README
KNOWN=" "

n=0; bad=0; kept=0
while read -r e; do
  b=$(basename "$e" .elf)
  n=$((n + 1))
  "${X}objcopy" -O binary "$e" "$O/$b.bin"
  th=$("${X}nm" "$e" | awk '$3 == "tohost" {print $1}')
  # 正常的一项不到一百万拍；串口按每位 16 拍出失败细节
  if "$SIM/Vtb" +flash="$O/jump.bin@0x100000" +sdram="$O/$b.bin@0" +tohost="0x$th" +uartdiv=16 +max=4000000 > "$O/$b.log" 2>&1; then
    [[ $KNOWN == *" $b"[[:space:]]* ]] && { echo "FAIL $b 是已知失败却过了"; bad=$((bad + 1)); }
  elif [[ $KNOWN == *" $b"[[:space:]]* ]]; then
    kept=$((kept + 1))
  else
    echo "FAIL $b：$(grep -a 'Register:\|Bad Value\|Expected Value\|tohost' "$O/$b.log" | tr -s ' ' | tr '\n' ' ')"
    bad=$((bad + 1))
  fi
done < <(find "$E" -name '*.elf' | sort)

echo "跑 $n 个，$bad 个不过，已知失败 $kept 个"
[ "$n" -gt 0 ] && [ "$bad" = 0 ] && [ "$kept" = "$(wc -w <<< "$KNOWN")" ]
