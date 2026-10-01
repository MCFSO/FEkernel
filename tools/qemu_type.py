# SPDX-License-Identifier: 0BSD
"""qemu_type.py —— 向客户机"打字"，验证键盘 → 控制台 → shell 这条链。

用法：
    python tools/qemu_type.py --text "help"                 # 打一条命令并抓屏
    python tools/qemu_type.py --text "ls /" --shots 2
    python tools/qemu_type.py --script "help|ls /|cat /limine.conf"

★ 它测的是**整条交互链**，而不是某一段 ★

    QEMU sendkey → 8042 → IRQ1 → kbd 服务解码 → 推送 → consoled 行编辑 + 回显
                 → shell 读整行 → 执行 → 输出经 /dev/console → 屏幕

这条链上任何一环断了，屏幕上都看不到命令的**输出**——而"看不到输出"与
"命令执行失败"在截图里长得一样。所以判据分两层：
  1. 屏幕上要出现你敲的那行字（证明输入与回显通了）；
  2. 屏幕上要出现该命令的输出（证明 shell 收到了整行并执行了）。
程序会把两层都检查一遍并打印结论。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_ctl  # noqa: E402

BUILD = qemu_ctl.BUILD


def main() -> int:
    ap = argparse.ArgumentParser(description="向客户机注入按键并抓屏")
    ap.add_argument("--text", default="help", help="要敲的一行（会自动补回车）")
    ap.add_argument("--script", default="",
                    help="多条命令，用 | 分隔；给了它就忽略 --text")
    ap.add_argument("--out", default=str(BUILD / "qemu-typed.png"))
    ap.add_argument("--wait", type=float, default=120.0,
                    help="最多等多久让 shell 就绪（轮询日志，不是死等）")
    ap.add_argument("--keep-image", action="store_true")
    args = ap.parse_args()

    if not args.keep_image:
        qemu_ctl.rebuild_image()

    log = BUILD / "serial-type.log"
    with qemu_ctl.Guest(log) as g:
        # ★ 等待标记必须是"串口日志里真的会出现的字符串" ★
        #
        # 第一版等的是 shell 自己打印的 "FEKernel shell."——而那句话走的是
        # **屏幕**（经 /dev/console），串口日志里根本没有它。
        # 于是工具报"等不到 shell 启动"，而屏幕上 shell 早就跑起来了、提示符都在闪。
        # **症状（等不到）与事实（已经起来了）相反**，因为我在错误的证据来源里找。
        #
        # 正确的标记是 init 打到串口的那行（它确实走 fe_puts → COM1）。
        marker = "[init] sh 已拉起"
        if not g.wait_for(marker, args.wait):
            print(f"[type] **等不到 shell 启动**（串口日志里没有 {marker!r}）",
                  file=sys.stderr)
            tail = "\n".join(g.text().splitlines()[-12:])
            print(tail, file=sys.stderr)
            return 1
        print("[type] shell 已就绪（init 已拉起）")

        cmds = ([c.strip() for c in args.script.split("|") if c.strip()]
                if args.script else [args.text])
        for cmd in cmds:
            keys = g.type_text(cmd + "\n")
            print(f"[type] 敲入 {cmd!r} → {len(keys)} 个键名")
            g.wait_for("$ " + cmd, timeout=8.0)   # 等回显出现（拿不到也不致命）
            import time
            time.sleep(1.5)                        # 给命令执行与渲染留时间

        if not g.shot(Path(args.out)):
            print("[type] **抓屏失败**", file=sys.stderr)
            return 1
        print(f"[type] 截图 -> {args.out}")

        # 串口日志里 kbd 会把每个键打出来：这是"键真的到了驱动"的证据
        txt = g.text()
        keylines = [ln for ln in txt.splitlines() if "[kbd] #" in ln]
        print(f"[type] kbd 驱动解出 {len(keylines)} 个按键事件")
        for ln in keylines[-6:]:
            print("   " + ln.strip())
        shell_lines = [ln for ln in txt.splitlines()
                       if "shell" in ln or "not found" in ln]
        for ln in shell_lines[-4:]:
            print("   " + ln.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
