#!/usr/bin/env bash
# 用 riscv-arch-test 的 ACT4 框架把 kianv-rv32ima/ 这份配置编成自检 ELF，打成一个包。
# 期望值由 Sail 模型算出、编进 ELF 里，DUT 只要跑到 tohost：1 是过，3 是不过（细节从串口打出）。
# 要的工具：uv、Ruby（≥ 3.2，带头文件）与 bundler、sail-riscv 0.15 的 sail_riscv_sim、riscv64-linux-gnu-gcc-15。
# 用法：build.sh <工作目录>；包是 <工作目录>/kianv-rv32ima-elfs.tar.gz，打出它的 sha256 好填 arch-test.pin
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
W=$(realpath -m "$1")
ACT=ec0fd59e552a26d70b6e0ee4d1d6940d4454ec49
mkdir -p "$W"
if [ "$(git -C "$W/act" rev-parse HEAD 2>/dev/null)" != "$ACT" ]; then
  rm -rf "$W/act"
  git init -q "$W/act"
  git -C "$W/act" fetch -q --depth 1 https://github.com/riscv-non-isa/riscv-arch-test.git "$ACT"
  git -C "$W/act" checkout -q FETCH_HEAD
fi
export PATH=$here:$PATH
rm -rf "$W/work"
make -C "$W/act" CONFIG_FILES="$here/kianv-rv32ima/test_config.yaml" WORKDIR="$W/work" FAST=True > "$W/build.log" 2>&1 \
  || { grep -a "✗\|rror" "$W/build.log" | tail -n 20; exit 1; }
E=$W/work/kianv-rv32ima/elfs
n=$(find "$E" -name '*.elf' | wc -l)
[ "$n" -gt 0 ] || { echo "一个 ELF 也没编出来"; exit 1; }
# ACT4 每次生成的操作数都是随机的，同一份配置两次编出的 ELF 不同，所以流水线用的是按摘要钉住的那一个包
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -C "$E" -cf - . | gzip -n > "$W/kianv-rv32ima-elfs.tar.gz"
echo "$n 个 ELF，$(sha256sum "$W/kianv-rv32ima-elfs.tar.gz" | cut -d' ' -f1)"
