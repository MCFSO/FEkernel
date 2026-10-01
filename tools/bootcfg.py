# SPDX-License-Identifier: 0BSD
"""bootcfg.py —— 修改 build/esp/limine.conf 里的**启动请求**。

启动请求指的是被启动那个条目 cmdline 上的两个标记：

    fek.update=1   本次启动执行一次 A/B 更新
    fek.reboot=1   本次启动立即重启（复位路径自检）

★ 为什么这件事要单独抽出来，而且必须由**建镜像的人**显式声明 ★

这两个标记是**实验状态**，而它们此前只写在 build/esp/limine.conf 里——
于是它变成了"藏在构建产物里的状态"：

- `run_qemu.py --ab-update` 写进去；
- `build.py` 重新生成 limine.conf 时会清掉；
- 但**直接调 mkfat.py 的工具不会**（mkfat 只把 esp 目录照抄进镜像）。

后果我刚刚亲身踩到：`tools/qemu_mouse.py` 直接调 mkfat 建镜像，
镜像里带着上一次留下的 `fek.update=1`，于是这次"测鼠标"的启动
**偷偷做了一次 A/B 更新并重启**——而 WHPX 下客户机复位后固件不回来，
表现成"卡住、鼠标服务没起来"。查了半天以为是鼠标驱动坏了。

修法不是"记得清理"，而是**让 mkfat.py 每次都把请求显式设成调用者要的值**
（`--update-request` / `--reboot-request`，不给就是不请求）。
这样状态不可能被继承，每个工具都必须说清楚"我要什么"。
"""

from __future__ import annotations

from pathlib import Path

TOKENS = (("fek.update=1", "update"), ("fek.reboot=1", "reboot"))


def set_boot_request(esp: Path, update: bool = False, reboot: bool = False,
                     verbose: bool = True) -> None:
    """把 esp/limine.conf 里**被启动条目**的启动请求设成指定的值。

    只改 default_entry 指向的那一个条目：
    两个条目都写的话就会变成"更新→切换→再更新→再切回来"，永远停不下来。
    请求是**一次**的动作，不是系统的属性。

    幂等：同一次调用既负责"加上"，也负责"擦掉上一次的"，所以连续调用
    （带标记/不带标记）得到的镜像一定符合最后一次调用的意图。"""
    conf = esp / "limine.conf"
    if not conf.is_file():
        return
    lines = conf.read_text(encoding="utf-8").splitlines()

    default = 1                      # Limine 的 default_entry 是 1 起算的
    for ln in lines:
        if ln.startswith("default_entry:"):
            default = int(ln.split(":", 1)[1].strip())

    seen = 0
    out: list[str] = []
    for ln in lines:
        if ln.strip().startswith("cmdline:"):
            seen += 1
            body = ln.split(":", 1)[1].strip()
            for tok, _ in TOKENS:
                body = body.replace(" " + tok, "")
            body = body.strip()
            if seen == default:
                if update:
                    body += " fek.update=1"
                if reboot:
                    body += " fek.reboot=1"
            indent = ln[: len(ln) - len(ln.lstrip())]
            out.append(f"{indent}cmdline: {body}")
        else:
            out.append(ln)
    conf.write_text("\n".join(out), encoding="utf-8")
    if verbose:
        print(f"[bootcfg] 启动请求：更新={'开' if update else '关'} "
              f"重启={'开' if reboot else '关'}（条目 {default}）")
