# SPDX-License-Identifier: 0BSD
"""ppm2png.py —— 把 QEMU screendump 产出的 PPM 转成 PNG。

QEMU 的 monitor 命令 `screendump` 只写 PPM（P6），而人眼（与图片查看器）
要的是 PNG。这个转换只用标准库：zlib + struct，不引入 Pillow。

用法：
    python tools/ppm2png.py build/qemu-screen.ppm [build/qemu-screen.png]
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


def read_ppm(p: Path) -> tuple[int, int, bytes]:
    data = p.read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit(f"[ppm2png] {p} 不是 P6 PPM（头部 {data[:2]!r}）")
    # 头部：P6 <空白> 宽 <空白> 高 <空白> 最大值 <单个空白字符> 然后是二进制数据。
    # PPM 允许 '#' 注释行，所以不能简单地 split()。
    fields: list[bytes] = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1  # 跳掉最大值后面那一个空白字符
    w, h, maxval = (int(f) for f in fields)
    if maxval != 255:
        raise SystemExit(f"[ppm2png] 只支持 8 位/通道，得到 maxval={maxval}")
    pixels = data[i : i + w * h * 3]
    if len(pixels) != w * h * 3:
        raise SystemExit(f"[ppm2png] 像素数据不足：{len(pixels)} != {w * h * 3}")
    return w, h, pixels


def write_png(p: Path, w: int, h: int, rgb: bytes) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # 每行的过滤器类型：0 = None
        raw += rgb[y * w * 3 : (y + 1) * w * 3]

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    p.write_bytes(png)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".png")
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb)
    print(f"[ppm2png] {src.name} ({w}x{h}) -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
