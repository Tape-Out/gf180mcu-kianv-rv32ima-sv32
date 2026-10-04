"""照 ip.yaml 的黑盒声明出 verilator 的参数：源文件、头文件目录、宏。

旋钮取默认值，命令行上的 <旋钮>=<值> 盖过它：测试矩阵的每一点由任务把解出的值传进来。
"""
import glob
import sys

import yaml

ip = yaml.safe_load(open("ip.yaml", encoding="utf-8"))
e = ip["emit"][0]
knob = {k: v["default"] for k, v in ip["params"].items()}
for a in sys.argv[1:]:
    k, v = a.split("=", 1)
    if k not in knob:
        sys.exit(f"没有旋钮 {k}")
    knob[k] = int(v)
out = [f"-D{m}={knob[v] if isinstance(v, str) else v}" for m, v in e["defines"].items()]
out += [f"-I{d}" for d in e["includes"]]
for r in e["rtl"]:
    out += sorted(glob.glob(r["glob"])) if isinstance(r, dict) else [r]
print(" ".join(out))
