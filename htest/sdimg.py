#!/usr/bin/env python3
"""出一张 SD 卡的镜像给测试台的 +sd 用：MBR 里一个分区，FAT16，根目录下放给定的几个文件。

用法：sdimg.py <输出> [<文件>...]      文件名要是 8.3 的；镜像 16 MiB
      sdimg.py --cat <镜像> <文件名>   把镜像里根目录下的一个文件打到标准输出（核对芯片写进去的东西）
只用标准库：流水线上不必装 dosfstools 与 mtools。文件在数据区里连着放，名字存成小写。
"""
import pathlib
import struct
import sys

SEC = 512
TOTAL = 16 * 1024 * 1024 // SEC
START = 64
NSEC = TOTAL - START
ROOT = 512
FATSZ = 128


def name83(n: str) -> bytes:
    base, _, ext = n.upper().partition(".")
    if not 0 < len(base) <= 8 or len(ext) > 3 or not (base + ext).replace("_", "").replace("-", "").isalnum():
        sys.exit(f"{n} 不是 8.3 的名字")
    return base.ljust(8).encode() + ext.ljust(3).encode()


def build(out: str, files: list[str]) -> None:
    img = bytearray(TOTAL * SEC)
    # MBR：一个 FAT16 分区，从第 64 扇区到末尾
    img[446:462] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x06, b"\xfe\xff\xff", START, NSEC)
    img[510:512] = b"\x55\xaa"
    p = START * SEC
    bpb = struct.pack("<3s8sHBHBHHBHHHII", b"\xeb\x3c\x90", b"TO2610  ", SEC, 1, 1, 2, ROOT, NSEC, 0xF8, FATSZ, 32, 64,
                      START, 0)
    bpb += struct.pack("<BBBI11s8s", 0x80, 0, 0x29, 0x26100001, b"NO NAME    ", b"FAT16   ")
    img[p:p + len(bpb)] = bpb
    img[p + 510:p + 512] = b"\x55\xaa"
    fat = bytearray(FATSZ * SEC)
    fat[0:4] = b"\xf8\xff\xff\xff"
    rootdir = bytearray(ROOT * 32)
    data0 = START + 1 + 2 * FATSZ + ROOT * 32 // SEC
    cl = 2
    for k, f in enumerate(files):
        body = pathlib.Path(f).read_bytes()
        n = max(1, -(-len(body) // SEC))
        for i in range(n):
            struct.pack_into("<H", fat, 2 * (cl + i), 0xFFFF if i == n - 1 else cl + i + 1)
        a = (data0 + cl - 2) * SEC
        img[a:a + len(body)] = body
        # 第 12 字节的 0x18：主名与扩展名都显示成小写
        rootdir[32 * k:32 * k + 32] = struct.pack("<11sBB5xHHHHHI", name83(pathlib.Path(f).name), 0x20, 0x18, 0, 0, 0x5B41,
                                                  0x5B41, cl, len(body))[:32]
        cl += n
    for i in range(2):
        a = (START + 1 + i * FATSZ) * SEC
        img[a:a + len(fat)] = fat
    a = (START + 1 + 2 * FATSZ) * SEC
    img[a:a + len(rootdir)] = rootdir
    pathlib.Path(out).write_bytes(img)


def cat(image: str, want: str) -> bytes:
    img = pathlib.Path(image).read_bytes()
    start = struct.unpack_from("<I", img, 446 + 8)[0]
    p = start * SEC
    spc, rsvd, nfat, root, fatsz = img[p + 13], *struct.unpack_from("<HBH", img, p + 14), struct.unpack_from("<H", img, p + 22)[0]
    fat = (start + rsvd) * SEC
    rootp = (start + rsvd + nfat * fatsz) * SEC
    data0 = rootp + root * 32
    for k in range(root):
        e = img[rootp + 32 * k:rootp + 32 * k + 32]
        if e[0] in (0, 0xE5) or e[11] & 0x08 or e[11] == 0x0F:
            continue
        if e[:11] == name83(want):
            cl, size = struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
            body = b""
            while 2 <= cl < 0xFFF0 and len(body) < size:
                a = data0 + (cl - 2) * spc * SEC
                body += img[a:a + spc * SEC]
                cl = struct.unpack_from("<H", img, fat + 2 * cl)[0]
            return body[:size]
    sys.exit(f"镜像里没有 {want}")


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--cat":
        sys.stdout.buffer.write(cat(sys.argv[2], sys.argv[3]))
    elif len(sys.argv) >= 2:
        build(sys.argv[1], sys.argv[2:])
    else:
        sys.exit(__doc__)
