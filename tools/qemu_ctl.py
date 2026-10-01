# SPDX-License-Identifier: 0BSD
"""qemu_ctl.py —— 启动 QEMU 并通过 monitor 与它对话（抓屏、注入键鼠的公共底座）。

★ 为什么要有这个模块 ★
`qemu_shot.py`（抓屏）、`qemu_mouse.py`（注入鼠标）、`qemu_type.py`（注入按键）
需要的是同一件事：**启动一台客户机，等它到达某个状态，然后通过 monitor 下命令**。
各自抄一遍启动参数意味着：加一个参数（比如 `-cpu host,-tsc-deadline`）
要改三处，漏一处就出现"只有某个工具跑不起来"的怪现象——
这与 fe_blk.h / fe_fs.h / fe_console.h 抽出来是同一个理由。

★ 等待用"状态"而不是"秒数" ★
`wait_for()` 轮询串口日志直到出现标记串。前面已经踩过两次：
VBox 截图抓早了、鼠标事件注入早了——两次的症状都伪装成"实现坏了"。
"""

from __future__ import annotations

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

_PORT_BASE = 5630
_next_port = [_PORT_BASE]


def alloc_port() -> int:
    p = _next_port[0]
    _next_port[0] += 1
    return p


def rebuild_image(update: bool = False, reboot: bool = False) -> None:
    """重建 disk.img。

    ★ 默认要重建 ★ `build.py` 只重建 build/esp，**不重建 disk.img**，
    改了用户态程序却直接跑 QEMU，跑的是旧二进制（截图也是旧的）。
    启动请求也在这里显式声明：mkfat 会把它归一化，所以上一次实验留下的
    标记不会被继承（见 tools/bootcfg.py）。"""
    args = [sys.executable, str(ROOT / "tools" / "mkfat.py"),
            str(RAW_IMG), str(BUILD / "esp"), "--label", "FEKERNEL"]
    if update:
        args.append("--update-request")
    if reboot:
        args.append("--reboot-request")
    subprocess.run(args, check=True, cwd=str(ROOT),
                   stdout=subprocess.DEVNULL)


class Guest:
    """一台正在跑的客户机 + 一条 monitor 连接。"""

    def __init__(self, log: Path, mem: str = "512M", smp: int = 1,
                 accel: str = "whpx", headless_display: bool = True):
        self.log = log
        if log.is_file():
            log.unlink()
        qemu = tc.find_qemu()
        code = tc.find_ovmf()
        self.port = alloc_port()
        cmd = [
            str(qemu), "-machine", "pc", "-accel", accel, "-m", mem,
            "-smp", str(smp),
            # -cpu host：让客户机看到宿主的 SIMD/XSAVE 特性；
            # 关掉 tsc-deadline：那条路径在 WHPX 下不投递中断（见 docs/08）
            "-cpu", "host,-tsc-deadline" if accel != "tcg" else "max,-tsc-deadline",
            "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
            "-drive", f"if=pflash,format=raw,file={BUILD / 'ovmf_vars.fd'}",
            "-drive", f"file={RAW_IMG},format=raw,if=ide,index=0,media=disk",
            "-serial", f"file:{log}",
            "-monitor", f"tcp:127.0.0.1:{self.port},server,nowait",
        ]
        if headless_display:
            cmd += ["-display", "none"]
        self.proc = subprocess.Popen(cmd, cwd=str(ROOT),
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)

    # ---- monitor ----
    def mon(self, command: str, settle: float = 0.4) -> None:
        """发一条 monitor 命令（每次新开连接，省得维护状态）。"""
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as s:
            s.sendall((command + "\n").encode())
            time.sleep(settle)

    def type_text(self, text: str, per_key: float = 0.06) -> list[str]:
        """把一段文本按 QEMU 的键名逐个 sendkey 进去。返回实际发出的键名列表。

        QEMU 的 sendkey 用的是 QKeyCode 名字（`a`、`spc`、`ret`、`slash`…），
        大写与上档符号要写 `shift-x`。这里做这层映射——
        没有它，测试里只能敲小写字母和数字，"打字"这件事就测不完整。"""
        keys = []
        for ch in text:
            name = char_to_key(ch)
            if name is None:
                continue
            keys.append(name)
        for k in keys:
            self.mon(f"sendkey {k}", settle=per_key)
        return keys

    def shot(self, out: Path, ppm: Path | None = None) -> bool:
        """抓屏并转 PNG。返回是否成功。"""
        import ppm2png
        ppm = ppm or out.with_suffix(".ppm")
        if ppm.is_file():
            ppm.unlink()
        self.mon(f"screendump {ppm}", settle=2.0)
        if not ppm.is_file():
            return False
        w, h, rgb = ppm2png.read_ppm(ppm)
        ppm2png.write_png(out, w, h, rgb)
        return True

    def wait_for(self, marker: str, timeout: float = 90.0) -> bool:
        """轮询串口日志直到出现 marker。★ 用状态做条件，不要用时间做条件 ★"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.log.is_file():
                try:
                    if marker in self.log.read_text(encoding="utf-8",
                                                    errors="replace"):
                        return True
                except OSError:
                    pass
            time.sleep(0.4)
        return False

    def text(self) -> str:
        if not self.log.is_file():
            return ""
        return self.log.read_text(encoding="utf-8", errors="replace")

    def close(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self) -> "Guest":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# QKeyCode 名字表：不够的键按需再加，加了要写清用途
_NAMED = {
    " ": "spc", "\n": "ret", "\r": "ret", "\b": "backspace", "\t": "tab",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    "\\": "backslash", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    ",": "comma", ".": "dot", "/": "slash",
}
_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal", "{": "bracket_left",
    "}": "bracket_right", "|": "backslash", ":": "semicolon", '"': "apostrophe",
    "~": "grave_accent", "<": "comma", ">": "dot", "?": "slash",
}


def char_to_key(ch: str) -> str | None:
    """一个字符 → QEMU sendkey 的键名。无法表示的返回 None。"""
    if ch in _NAMED:
        return _NAMED[ch]
    if "a" <= ch <= "z" or "0" <= ch <= "9":
        return ch
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    if ch in _SHIFTED:
        return "shift-" + _SHIFTED[ch]
    return None
