# SPDX-License-Identifier: 0BSD
"""abtool —— 在**主机侧、离盘**查看和修改 disk.img 里的 A/B 状态。

为什么需要它：

1. **诊断**。"现在这块盘认为下次该启动哪个槽""槽状态记录里 attempts 是几"
   这两个问题在系统跑不起来的时候恰恰最需要答案——而那时唯一的办法就是
   把盘拿下来直接看。等系统起来了再问它，就晚了。
2. **把回滚路径变成可测的**。回滚的触发条件是"试用启动连续失败 3 次"，
   要自然制造它得先让一个槽真的坏掉、再启动它 4 次（4 轮完整启动 ≈ 8 分钟）。
   而那个条件在盘上就是**几个字节**：把 attempts 直接置成 3，
   下一次启动就会走到回滚判定。**被种下的是历史，做出判断的是真正的状态机**——
   这两件事不能混为一谈，所以本工具的 `seed` 只写盘点状态，不含任何判断逻辑。

布局（misc 与 bootsel 的 LBA）不硬编码：从镜像里的 /etc/protect.list 里读，
那份清单是 mkfat.py 建镜像时按**这一次的布局**生成的。

用法：
    python tools/abtool.py show
    python tools/abtool.py slots
    python tools/abtool.py bootsel a|b
    python tools/abtool.py seed --active b --successful 0 --attempts 3
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

SECTOR = 512
AB_MAGIC = 0x4C534546            # 'FESL'
BOOTSEL = {"a": ord("1"), "b": ord("2")}   # default_entry 是 1 起算的

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IMG = ROOT / "build" / "disk.img"


def load(img: Path) -> bytearray:
    if not img.is_file():
        print(f"[错误] 找不到 {img}，先运行: python tools/build.py && "
              f"python tools/mkfat.py build/disk.img build/esp --label FEKERNEL",
              file=sys.stderr)
        raise SystemExit(1)
    return bytearray(img.read_bytes())


def parse_manifest(data: bytearray) -> dict:
    """从镜像里的 /etc/protect.list 取出这次布局的关键位置。

    注意：`find(b"misc ")` 会命中清单**注释里**的示例行（"# misc <lba> <count>"），
    而且命中点在一行中间——于是切出来的第一"行"没有 '#' 前缀，看起来像数据行。
    第一版就栽在这里（`int('<lba>')` 报错）。所以两步都要做：
    回退到上一个换行符对齐行首，并且**按能否解析成整数**来决定要不要采信。
    解析失败就当注释跳过，而不是崩掉：这个工具的用途正是"系统起不来时看盘",
    那时候它自己更不能因为一行文本就退出。"""
    i = data.find(b"misc ")
    if i < 0:
        print("[错误] 镜像里找不到 A/B 清单", file=sys.stderr)
        raise SystemExit(1)
    start = data.rfind(b"\n", max(0, i - 4096), i)
    start = 0 if start < 0 else start + 1
    text = bytes(data[start:start + 8192]).split(b"\x00")[0].decode("utf-8", "replace")
    out: dict = {"slots": {}}
    for line in text.splitlines():
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] == "slot" and len(p) >= 4:
            # 槽行的第二个字段是字母 a/b，**不是数字**——所以它不能走下面
            # 那条"整行都得能解析成整数"的路（第一版就是这么被跳过的，
            # 表现是 slots 子命令一条区间都不打印）。
            try:
                out["slots"].setdefault(p[1], []).append((int(p[2]), int(p[3])))
            except ValueError:
                continue
            continue
        try:
            nums = [int(x) for x in p[1:]]
        except ValueError:
            continue                      # 注释里的示例行
        if p[0] == "misc" and len(nums) >= 2:
            out["misc_lba"], out["misc_count"] = nums[0], nums[1]
        elif p[0] == "bootsel" and len(nums) >= 2:
            out["bootsel_lba"], out["bootsel_off"] = nums[0], nums[1]
    return out


def record_bytes(seq: int, active: int, successful: int, attempts: int) -> bytes:
    """与 mkfat.py 的 slot_state_bytes 逐字节一致：前 12 字节参与 CRC。"""
    body = struct.pack("<IIBBBB", AB_MAGIC, seq, active, successful, attempts, 0)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return (body + struct.pack("<I", crc)).ljust(SECTOR, b"\x00")


def read_record(data: bytearray, lba: int, copy: int) -> dict:
    off = (lba + copy) * SECTOR
    raw = bytes(data[off:off + SECTOR])
    magic, seq, active, successful, attempts, pad = struct.unpack("<IIBBBB", raw[:12])
    crc, = struct.unpack("<I", raw[12:16])
    return {
        "magic": magic, "seq": seq, "active": active, "successful": successful,
        "attempts": attempts, "crc": crc,
        "crc_ok": magic == AB_MAGIC and crc == (zlib.crc32(raw[:12]) & 0xFFFFFFFF),
    }


def cmd_show(data: bytearray, lay: dict) -> int:
    lba = lay["misc_lba"]
    print(f"misc   : LBA {lba} + {lay['misc_count']}")
    print(f"bootsel: LBA {lay['bootsel_lba']} 偏移 {lay['bootsel_off']}")
    cur = data[lay["bootsel_lba"] * SECTOR + lay["bootsel_off"]]
    slot = {ord("1"): "A", ord("2"): "B"}.get(cur, f"?({cur!r})")
    print(f"引导控制块: {chr(cur)!r} -> 下次启动槽 {slot}")
    best = None
    for c in (0, 1):
        r = read_record(data, lba, c)
        ok = "CRC OK" if r["crc_ok"] else "**CRC 坏**"
        print(f"  记录份 {c}: magic {'OK' if r['magic'] == AB_MAGIC else '**坏**'}, "
              f"{ok}, seq={r['seq']}, active={'AB'[r['active']] if r['active'] < 2 else '?'}, "
              f"successful={r['successful']}, attempts={r['attempts']}")
        if r["crc_ok"] and (best is None or r["seq"] > best[1]["seq"]):
            best = (c, r)
    if best:
        c, r = best
        print(f"  => 有效记录是第 {c} 份：下次启动槽 "
              f"{'AB'[r['active']] if r['active'] < 2 else '?'}，"
              f"{'已确认可用' if r['successful'] else '试用中'}，"
              f"已失败 {r['attempts']} 次")
    else:
        print("  => 两份记录都无效（下次启动会按当前槽重建）")
    return 0


def cmd_slots(data: bytearray, lay: dict) -> int:
    for name in sorted(lay["slots"]):
        ex = lay["slots"][name]
        total = sum(n for _l, n in ex)
        print(f"槽 {name.upper()}: {len(ex)} 段，共 {total} 扇区 "
              f"({total * SECTOR // 1024} KiB)")
        for lba, n in ex:
            print(f"    LBA {lba} + {n}")
    a, b = lay["slots"].get("a", []), lay["slots"].get("b", [])
    if len(a) == len(b) and all(x[1] == y[1] for x, y in zip(a, b)):
        print("两个槽逐段对齐（段数与长度都相同）—— 可直接逐段拷贝")
    else:
        print("**两个槽不对齐** —— 逐段拷贝不可用")
    return 0


def cmd_bootsel(data: bytearray, lay: dict, slot: str) -> int:
    off = lay["bootsel_lba"] * SECTOR + lay["bootsel_off"]
    old = chr(data[off])
    data[off] = BOOTSEL[slot]
    print(f"引导控制块: {old!r} -> {chr(BOOTSEL[slot])!r}（下次启动槽 {slot.upper()}）")
    return 0


def cmd_seed(data: bytearray, lay: dict, args) -> int:
    lba = lay["misc_lba"]
    active = 0 if args.active == "a" else 1
    # 挑一份写：默认写"另一份"，这样被替代的那份还在，正好也演示了双份的意义
    copies = [args.copy] if args.copy >= 0 else [0, 1]
    seq = max(read_record(data, lba, c)["seq"] for c in (0, 1)) + 1
    rec = record_bytes(seq, active, args.successful, args.attempts)
    for c in copies:
        off = (lba + c) * SECTOR
        data[off:off + SECTOR] = rec
    print(f"已种入槽状态: seq={seq}, active={args.active.upper()}, "
          f"successful={args.successful}, attempts={args.attempts}"
          f"（写到第 {'/'.join(str(c) for c in copies)} 份）")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="离盘查看/修改 A/B 状态")
    ap.add_argument("--img", type=Path, default=DEFAULT_IMG)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show", help="打印引导控制块与两份槽状态记录")
    sub.add_parser("slots", help="打印两个槽的区间列表")
    p = sub.add_parser("bootsel", help="直接改引导控制块（a 或 b）")
    p.add_argument("slot", choices=["a", "b"])
    p = sub.add_parser("seed", help="种入一份槽状态记录（用于构造回滚场景）")
    p.add_argument("--active", choices=["a", "b"], required=True)
    p.add_argument("--successful", type=int, choices=[0, 1], default=0)
    p.add_argument("--attempts", type=int, default=0)
    p.add_argument("--copy", type=int, choices=[0, 1], default=-1,
                   help="写到第几份（默认两份都写）")
    args = ap.parse_args()

    data = load(args.img)
    lay = parse_manifest(data)
    if "misc_lba" not in lay or "bootsel_lba" not in lay:
        print("[错误] 清单里缺少 misc / bootsel 行", file=sys.stderr)
        return 1

    if args.cmd == "show":
        rc = cmd_show(data, lay)
    elif args.cmd == "slots":
        rc = cmd_slots(data, lay)
    elif args.cmd == "bootsel":
        rc = cmd_bootsel(data, lay, args.slot)
    else:
        rc = cmd_seed(data, lay, args)

    if args.cmd in ("bootsel", "seed"):
        args.img.write_bytes(bytes(data))
        print(f"已写回 {args.img}")
        cmd_show(bytearray(args.img.read_bytes()), lay)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
