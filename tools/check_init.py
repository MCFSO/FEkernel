# SPDX-License-Identifier: 0BSD
"""核对 init 重建：把新日志与参考日志的**结构**逐项对齐。

★ 为什么需要一个专门的核对脚本（而不是"看一眼日志"）★
init 被误截断过一次，重建的正确性不能靠"跑起来没崩"来判断——
它要证明的是若干条**可枚举的结构事实**：24 个单元、算出来的启动顺序、
每个单元都给出了结果行、以及最终失败项为 0。这些都能从两份日志里
机械地抽出来对比，所以我把它写成脚本，而不是用眼睛看。

用法：
    python tools/check_init.py build/serial.log            # 与默认参考对比
    python tools/check_init.py build/serial.log --ref build/vbox-serial.log
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REF = ROOT / "build" / "serial-ref.txt"


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def unit_table(text: str) -> tuple[list[str], set[str], set[str]]:
    """抽出单元表：名字、以及哪些是**常驻服务** / **动作型单元**。

    ★ 为什么要连"常驻"一起抽出来 ★
    常驻服务（daemon）与动作型单元**没有退出码行**——它们的成败由
    "有没有发布名字"和"动作自己报的失败项"表达。把"没有结果行"一律当错，
    会让核对脚本对 8 个正常的常驻服务报假失败（第一版就是这样，
    而假失败会让人开始忽略这个脚本——那比没有脚本更糟）。"""
    m = re.search(r"\[init\] 单元表: 声明 (\d+) 个\n(.*?)\n\[init\] 解析出启动顺序",
                  text, re.S)
    if not m:
        return [], set(), set()
    decl = int(m.group(1))
    names: list[str] = []
    daemons: set[str] = set()
    actions: set[str] = set()
    for line in m.group(2).splitlines():
        mm = re.match(r"\s*\d+\.\s+(\S+)", line)
        if not mm:
            continue
        name = mm.group(1)
        names.append(name)
        if "[常驻]" in line:
            daemons.add(name)
        if "[动作" in line:
            actions.add(name)
    if len(names) != decl:
        raise SystemExit(f"单元表行数与声明数不符：{len(names)} != {decl}")
    return names, daemons, actions


def start_order(text: str) -> list[str]:
    m = re.search(r"\[init\] 解析出启动顺序: (.*)", text)
    if not m:
        return []
    return [x.strip() for x in m.group(1).split("→")]


def unit_results(text: str) -> dict[str, str]:
    """每个单元的结果行：退出码 / 跳过（分两种原因）/ 未就绪。

    ★ 两种"跳过"必须分开记 ★
    它们长得一样（都没跑），含义完全相反：
      - "负向测试的预期结果" —— 这条**是**一个测试结论，它跑对了；
      - "硬件前提不成立"     —— 这台机器上根本没有那样硬件，
                                它不是结论、也不是缺陷。
    混成一个 `skipped` 之后，工具就无法回答"这次跳过是我预期的吗"。"""
    out: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"\[init\] (\S+) 退出码 = (-?\d+)(.*)", line)
        if m:
            out[m.group(1)] = "exit=" + m.group(2) + (" 不符" if "不符" in m.group(3) else "")
            continue
        m = re.match(r"\[init\] 跳过 (\S+)：(.*)", line)
        if m:
            out[m.group(1)] = "skipped-hw" if "硬件前提不成立" in m.group(2) else "skipped"
    return out


def summarize(text: str) -> dict:
    tail_fail = None
    for line in text.splitlines():
        m = re.match(r"=== init 结束，失败项 (\d+) ===", line)
        if m:
            tail_fail = int(m.group(1))
    names, daemons, actions = unit_table(text)
    return {
        "units": names,
        "daemons": daemons,
        "actions": actions,
        "order": start_order(text),
        "results": unit_results(text),
        "init_fail": tail_fail,
        "milestone_ok": "M5 里程碑自检全部通过" in text,
        "has_failure_line": "自检存在" in text,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="核对 init 重建的结构事实")
    ap.add_argument("log", type=Path)
    ap.add_argument("--ref", type=Path, default=None,
                    help="参考日志（默认 build/serial-ref.txt）")
    args = ap.parse_args()

    new = summarize(read(args.log))
    bad = 0

    print(f"[check] 单元表: {len(new['units'])} 个")
    if len(new["units"]) != 28:
        print(f"  ** 期望 28 个单元，实际 {len(new['units'])} **")
        bad += 1
    print(f"[check] 启动顺序: {' → '.join(new['order']) or '(缺失)'}")
    if not new["order"]:
        print("  ** 没有解析出启动顺序 **")
        bad += 1
    print(f"[check] init 失败项: {new['init_fail']}")
    if new["init_fail"] != 0:
        print("  ** init 报告的失败项不是 0 **")
        bad += 1
    print(f"[check] 里程碑自检全部通过: {new['milestone_ok']}")
    if not new["milestone_ok"]:
        print("  ** 整机自检没有全部通过 **")
        bad += 1

    # 一次性程序必须有退出码行；常驻服务与动作型单元本来就没有
    # （它们的成败由"发布名字了没有"与"动作自己报的失败项"表达）。
    # 因本机无此硬件而跳过的单元也没有——它根本没被启动。
    expect_result = [u for u in new["units"]
                     if u not in new["daemons"] and u not in new["actions"]
                     and new["results"].get(u) != "skipped-hw"]
    missing = [u for u in expect_result if u not in new["results"]]
    if missing:
        print(f"  ** 这些一次性程序没有结果行: {missing} **")
        bad += 1
    else:
        print(f"[check] {len(expect_result)} 个一次性程序都有退出码行"
              f"（{len(new['daemons'])} 个常驻 + {len(new['actions'])} 个动作型不需要）")
    skipped = [u for u, r in new["results"].items() if r == "skipped"]
    if skipped:
        print(f"[check] 被跳过的单元（负向测试的预期结果）: {skipped}")
    skipped_hw = [u for u, r in new["results"].items() if r == "skipped-hw"]
    if skipped_hw:
        print(f"[check] 因本机无此硬件而跳过的单元（不算失败）: {skipped_hw}")
    mismatched = [u for u, r in new["results"].items() if "不符" in r]
    if mismatched:
        print(f"  ** 退出码与期望不符的单元: {mismatched} **")
        bad += 1

    ref_path = args.ref or DEFAULT_REF
    if ref_path.is_file():
        ref = summarize(read(ref_path))
        # ★ 这两条一旦不一致，"一致"那句话就一个字都不能说 ★
        # 第一版把成功提示写在了 `if ref["order"]:` 里面，于是它会**紧接着
        # 两条不一致的告警**打印"单元表与启动顺序均与参考一致"——
        # 一份自相矛盾的报告比没有报告更糟：看最后一行的人会以为没事。
        same = True
        if ref["units"] and ref["units"] != new["units"]:
            print("  ** 单元表与参考不一致 **")
            print(f"     参考: {ref['units']}")
            print(f"     实际: {new['units']}")
            bad += 1
            same = False
        if ref["order"] and ref["order"] != new["order"]:
            print("  ** 启动顺序与参考不一致 **")
            print(f"     参考: {' → '.join(ref['order'])}")
            print(f"     实际: {' → '.join(new['order'])}")
            bad += 1
            same = False
        if same:
            print("[check] 单元表与启动顺序均与参考一致")
    else:
        print(f"[check] （参考日志 {ref_path} 不存在，跳过逐项对比）")

    print("=> " + ("核对通过" if bad == 0 else f"核对发现 {bad} 项问题"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
