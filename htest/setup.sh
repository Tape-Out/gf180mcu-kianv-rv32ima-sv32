#!/usr/bin/env bash
# 拼出 build/src：上游的源码原样拷一份，套上 patch/ 下的补丁。子模块本身不动，上游修了哪一处就撤哪一个补丁。
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf build/src
mkdir -p build
cp -r third_party/gf180mcu-kianv-rv32ima-sv32/src build/src
for p in patch/*.patch; do
  patch -s -p1 -d build < "$p"
done
echo "build/src：上游源码加 $(ls patch/*.patch | wc -l) 个补丁"
