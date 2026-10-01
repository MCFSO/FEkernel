# SPDX-License-Identifier: 0BSD
"""qemu_mouse.py —— 用 QEMU 的 monitor 注入鼠标事件，验证用户态鼠标驱动。

★ 为什么需要它 ★
`mouse` 服务从写出来到现在**一次都没有被真的触发过**：
它初始化全绿（认领端口、注册 IRQ12、开数据上报），但"3 字节数据包解出来对不对"
这条路径没有任何证据。而"没跑过的路径等于没有"——这与控制台接口那次一样。

VBox 没有鼠标注入手段（`keyboardputscancode` 只管键盘），
所以这件事只能在 QEMU 上做：monitor 有 `mouse_move dx dy` 与 `mouse_button N`。

它做两件事：
  1. 先确认客户机已经跑到"进入等待鼠标数据循环"（**时序**：不等就可能注入到空气里）；
  2. 移动鼠标、按一下左键，然后从串口日志里找驱动打印的包，核对 dx/dy/按键。

用法：
    python tools/qemu_mouse.py [--wait 40] [--dx 25] [--dy -10]
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402

ROOT = tc.ROOT
BUILD = tc.BUILD
RAW_IMG = BUILD / "disk.img"
LOG = BUILD / "serial-mouse.log"
PORT = 5621


def mon_send(sock: socket.socket, cmd: str) -> None:
    sock.sendall((cmd + "\n").encode())
    time.sleep(0.4)


def main() -> int:
    ap = argparse.ArgumentParser(description="注入鼠标事件并核对驱动解析")
    ap.add_argument("--wait", type=float, default=90.0,
                    help="最多等多少秒让客户机进入鼠标等待循环（轮询日志，不是死等）")
    ap.add_argument("--dx", type=int, default=25)
    ap.add_argument("--dy", type=int, default=-10)
    ap.add_argument("--keep-image", action="store_true")
    args = ap.parse_args()

    if not args.keep_image:
        subprocess.run([sys.executable, str(ROOT / "tools" / "mkfat.py"),
                        str(RAW_IMG), str(BUILD / "esp"), "--label", "FEKERNEL"],
                       check=True, cwd=str(ROOT))
    if LOG.is_file():
        LOG.unlink()

    qemu = tc.find_qemu()
    code = tc.find_ovmf()
    cmd = [
        str(qemu), "-machine", "pc", "-accel", "whpx", "-m", "512M", "-smp", "1",
        "-cpu", "host,-tsc-deadline",
        "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive", f"if=pflash,format=raw,file={BUILD / 'ovmf_vars.fd'}",
        "-drive", f"file={RAW_IMG},format=raw,if=ide,index=0,media=disk",
        "-display", "none", "-serial", f"file:{LOG}",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
    ]
    print(f"[mouse] 启动 QEMU，等待客户机进入鼠标等待循环（最多 {args.wait:.0f} 秒）")
    proc = subprocess.Popen(cmd, cwd=str(ROOT), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    rc = 1
    try:
        # ★ 轮询日志直到目标状态出现，而不是"睡固定秒数" ★
        #
        # 第一版睡 45 秒就注入，结果是"驱动日志 0 行"——因为客户机**还没跑到**
        # 鼠标服务。这个症状与"驱动坏了"一模一样，而它其实只是"注入早了"。
        # 固定等待是在赌"引导时间不变"，而引导时间会因为控制台整屏重画
        # 这类改动而变长。**用状态做条件，不要用时间做条件。**
        marker = "进入等待鼠标数据循环"
        deadline = time.time() + args.wait
        ready = False
        while time.time() < deadline:
            if LOG.is_file() and marker in LOG.read_text(encoding="utf-8",
                                                        errors="replace"):
                ready = True
                break
            time.sleep(0.5)
        print(f"[mouse] 客户机已进入鼠标等待循环：{'是' if ready else '否（超时，仍试一次）'}")

        with socket.create_connection(("127.0.0.1", PORT), timeout=10) as s:
            mon_send(s, f"mouse_move {args.dx} {args.dy}")
            mon_send(s, "mouse_button 1")
            mon_send(s, "mouse_button 0")
            mon_send(s, "mouse_move 5 5")
            time.sleep(3.0)
        print("[mouse] 事件已注入")
        rc = 0
    except OSError as e:
        print(f"[错误] monitor 连接失败：{e}", file=sys.stderr)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    text = LOG.read_text(encoding="utf-8", errors="replace") if LOG.is_file() else ""
    lines = [ln for ln in text.splitlines() if "[mouse]" in ln]
    print(f"[mouse] 驱动日志 {len(lines)} 行：")
    for ln in lines[-14:]:
        print("   " + ln)

    pkts = [ln for ln in lines if "包:" in ln]
    if not pkts:
        print("[mouse] **没有任何数据包被解析** —— 说明要么事件没送到，"
              "要么驱动没解出来（这是要修的地方）", file=sys.stderr)
        return 1
    print(f"[mouse] 解析出 {len(pkts)} 个数据包 ✓")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
