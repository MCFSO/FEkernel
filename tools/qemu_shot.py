# SPDX-License-Identifier: 0BSD
"""qemu_shot.py —— 启动 QEMU、等一会儿、从 monitor 抓一张屏幕截图（PNG）。

为什么需要它：控制台服务是**画在屏幕上的**，而串口日志只能证明"它以为自己画上了"。
人眼（与验收）要的是屏幕本身。QEMU 的 monitor 命令 `screendump` 只写 PPM，
所以顺带用 ppm2png 转成 PNG。

★ 为什么不用 run_qemu.py 里那条路径 ★
那条路径是"跑固定秒数然后 kill"，没有 monitor 通道；
而抓屏需要**在客户机还活着的时候**发一条 monitor 命令。

用法：
    python tools/qemu_shot.py --wait 45                 # 45 秒后抓屏
    python tools/qemu_shot.py --wait 30 --out shot.png
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
import ppm2png  # noqa: E402

ROOT = tc.ROOT
BUILD = tc.BUILD
RAW_IMG = BUILD / "disk.img"
PORT = 5611


def main() -> int:
    ap = argparse.ArgumentParser(description="QEMU 抓屏")
    ap.add_argument("--wait", type=float, default=45.0, help="启动后多少秒抓屏")
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--out", default=str(BUILD / "qemu-screen.png"))
    ap.add_argument("--ppm", default=str(BUILD / "qemu-screen.ppm"))
    ap.add_argument("--keep-image", action="store_true",
                    help="复用现有 disk.img。★ 默认会重建 ★ —— "
                         "`build.py` 只重建 build/esp，**不重建 disk.img**，"
                         "所以改完用户态程序不重建镜像的话，"
                         "跑起来的是旧二进制，截出来的图也是旧的"
                         "（我第一版就是这么被骗过一次：改了屏显字符串，"
                         "截图却一模一样，还以为是改动没生效）")
    args = ap.parse_args()

    if not args.keep_image:
        print("[shot] 重建 disk.img（--keep-image 可跳过）")
        subprocess.run([sys.executable, str(ROOT / "tools" / "mkfat.py"),
                        str(RAW_IMG), str(BUILD / "esp"), "--label", "FEKERNEL"],
                       check=True, cwd=str(ROOT))

    qemu = tc.find_qemu()
    code = tc.find_ovmf()
    log = BUILD / "serial-shot.log"
    if log.is_file():
        log.unlink()

    cmd = [
        str(qemu), "-machine", "pc", "-accel", "whpx", "-m", args.mem,
        "-smp", str(args.smp), "-cpu", "host,-tsc-deadline",
        "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive", f"if=pflash,format=raw,file={BUILD / 'ovmf_vars.fd'}",
        "-drive", f"file={RAW_IMG},format=raw,if=ide,index=0,media=disk",
        "-display", "none",
        "-serial", f"file:{log}",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
    ]
    print(f"[shot] 启动 QEMU，{args.wait:.0f} 秒后抓屏")
    proc = subprocess.Popen(cmd, cwd=str(ROOT), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        time.sleep(args.wait)
        with socket.create_connection(("127.0.0.1", PORT), timeout=10) as s:
            s.sendall(f"screendump {args.ppm}\n".encode())
            time.sleep(2.0)          # 给 QEMU 落盘的时间
        print("[shot] screendump 已发送")
    except OSError as e:
        print(f"[错误] monitor 连接失败：{e}", file=sys.stderr)
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    ppm = Path(args.ppm)
    if not ppm.is_file():
        print("[错误] 没有生成 PPM（客户机可能还没进图形模式）", file=sys.stderr)
        return 1
    w, h, rgb = ppm2png.read_ppm(ppm)
    ppm2png.write_png(Path(args.out), w, h, rgb)
    print(f"[shot] {args.out} ({w}x{h})")

    if log.is_file():
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        print(f"[shot] 串口日志 {len(lines)} 行（{log}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
