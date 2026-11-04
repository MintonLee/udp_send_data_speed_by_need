#!/usr/bin/env python3
"""
gen_data.py — 生成用于 UDP 发送测试的随机二进制文件。

按指定大小生成一个填满随机字节的文件，可直接作为 udp_send_data_speed_by_need 的输入
（config.ini 中 data_source=file, data_file=<输出路径>）。仅用标准库。

示例：
  python3 scripts/gen_data.py                          # data/sample.bin, 128 MB
  python3 scripts/gen_data.py -o data/test.dat -s 128MB
  python3 scripts/gen_data.py -s 64M --seed 42         # 可复现（需 Python 3.9+）
  python3 scripts/gen_data.py -s 1048576               # 大小也可直接用字节数
"""

import argparse
import os
import random
import re
import sys
import time


def parse_size(text):
    """把 '128MB' / '128M' / '1G' / '512KB' / '1048576' 解析为字节数(int)。"""
    m = re.fullmatch(r"\s*([0-9]*\.?[0-9]+)\s*([KMGT]?)[IB]*\s*", str(text).upper())
    if not m:
        raise argparse.ArgumentTypeError("无法解析的大小: %r（示例：128MB / 1G / 1048576）" % text)
    num, unit = float(m.group(1)), m.group(2)
    mult = {"": 1, "K": 1024, "M": 1024 ** 2, "G": 1024 ** 3, "T": 1024 ** 4}[unit]
    size = int(num * mult)
    if size <= 0:
        raise argparse.ArgumentTypeError("大小必须大于 0")
    return size


def main():
    ap = argparse.ArgumentParser(
        description="生成随机二进制文件，用于 udp_send_data_speed_by_need 发送测试。")
    ap.add_argument("-o", "--output", default="data/sample.bin",
                    help="输出文件路径（默认 data/sample.bin）")
    ap.add_argument("-s", "--size", type=parse_size, default="128MB",
                    help="文件大小，如 128MB / 128M / 134217728（默认 128MB）")
    ap.add_argument("--chunk", type=parse_size, default="1MB",
                    help="每次写入的块大小（默认 1MB）")
    ap.add_argument("--seed", type=int, default=None,
                    help="可选随机种子，用于生成可复现的数据（需 Python 3.9+）")
    args = ap.parse_args()

    total = args.size
    chunk = args.chunk if args.chunk > 0 else 1024 * 1024

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    rng = random.Random(args.seed) if args.seed is not None else None
    if rng is not None and not hasattr(rng, "randbytes"):
        print("[ERR] --seed 需要 Python 3.9+（random.randbytes）。请升级或不带 --seed 运行。",
              file=sys.stderr)
        return 1

    print("[..] 正在生成 %s（%.2f MB，块大小 %.2f MB，随机源=%s）"
          % (args.output, total / 1024 / 1024, chunk / 1024 / 1024,
             "seed=%d" % args.seed if rng else "os.urandom"))

    written = 0
    t0 = time.time()
    with open(args.output, "wb") as f:
        while written < total:
            n = min(chunk, total - written)
            f.write(rng.randbytes(n) if rng is not None else os.urandom(n))
            written += n
            el = time.time() - t0
            rate = written / el / 1024 / 1024 if el > 0 else 0
            sys.stdout.write("\r  %3d%%  %.1f/%.1f MB  %.0f MB/s"
                             % (written * 100 // total,
                                written / 1024 / 1024, total / 1024 / 1024, rate))
            sys.stdout.flush()

    el = time.time() - t0
    print("\n[OK] 已生成: %s  大小=%d 字节 (%.2f MB)  耗时=%.1fs"
          % (args.output, total, total / 1024 / 1024, el))
    print("[提示] 在 config.ini 中设置 data_source=file, data_file=%s 后即可用 ./run.sh 发送。"
          % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
