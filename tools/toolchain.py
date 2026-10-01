# SPDX-License-Identifier: 0BSD
"""FEKernel 工具链发现与命令执行。

设计约束（来自项目要求）：
  * 纯 Windows 原生：不依赖 WSL / Docker / MSYS / Cygwin / make。
  * 优先使用项目内便携工具链 toolchain/，其次才是系统 PATH。
  * 所有路径发现失败时给出**可操作**的错误信息，而不是抛一个找不到文件的栈。
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parent.parent
TOOLCHAIN = ROOT / "toolchain"
BUILD = ROOT / "build"


class ToolchainError(RuntimeError):
    """工具链缺失或调用失败。"""


def _first_file(candidates: Iterable[Path | str | None]) -> Path | None:
    for c in candidates:
        if not c:
            continue
        p = Path(c)
        if p.is_file():
            return p
    return None


def _search_dirs(names: Sequence[str]) -> Path | None:
    for n in names:
        p = shutil.which(n)
        if p:
            return Path(p)
    return None


# --------------------------------------------------------------------------- #
# 单个工具的定位
# --------------------------------------------------------------------------- #

def find_clang() -> Path:
    p = _first_file([
        TOOLCHAIN / "llvm" / "bin" / "clang.exe",
        TOOLCHAIN / "llvm" / "bin" / "clang",
        _search_dirs(["clang.exe", "clang"]),
        Path(r"C:\Program Files\LLVM\bin\clang.exe"),
    ])
    if not p:
        raise ToolchainError(
            "找不到 clang 编译器。\n"
            f"  期望位置：{TOOLCHAIN / 'llvm' / 'bin' / 'clang.exe'}\n"
            "  修复：运行  python tools/fetch_toolchain.py"
        )
    return p


def find_ld_lld() -> Path:
    p = _first_file([
        TOOLCHAIN / "llvm" / "bin" / "ld.lld.exe",
        TOOLCHAIN / "llvm" / "bin" / "ld.lld",
        _search_dirs(["ld.lld.exe", "ld.lld"]),
        Path(r"C:\Program Files\LLVM\bin\ld.lld.exe"),
    ])
    if not p:
        raise ToolchainError("找不到 ld.lld 链接器。修复：python tools/fetch_toolchain.py")
    return p


def find_llvm_tool(name: str) -> Path:
    exe = name + (".exe" if os.name == "nt" else "")
    p = _first_file([
        TOOLCHAIN / "llvm" / "bin" / exe,
        _search_dirs([exe, name]),
        Path(r"C:\Program Files\LLVM\bin") / exe,
    ])
    if not p:
        raise ToolchainError(f"找不到 {name}。修复：python tools/fetch_toolchain.py")
    return p


def find_nasm() -> Path:
    p = _first_file([
        TOOLCHAIN / "nasm" / "nasm.exe",
        _search_dirs(["nasm.exe", "nasm"]),
        Path(r"C:\Program Files\NASM\nasm.exe"),
        Path(r"C:\Program Files (x86)\NASM\nasm.exe"),
    ])
    if not p:
        raise ToolchainError(
            "找不到 NASM 汇编器。\n"
            "  本机已安装位置应为 C:\\Program Files\\NASM\\nasm.exe\n"
            "  修复：安装 NASM（winget install NASM.NASM）或把它放进 toolchain/nasm/"
        )
    return p


def find_limine_dir() -> Path:
    """Limine 引导器的共享目录（含 BOOTX64.EFI / limine-bios.sys 等）。"""
    for cand in (TOOLCHAIN / "limine", TOOLCHAIN / "limine" / "limine-binary"):
        if (cand / "BOOTX64.EFI").is_file():
            return cand
    raise ToolchainError(
        "找不到 Limine 引导文件 (BOOTX64.EFI)。\n"
        "  修复：python tools/fetch_toolchain.py"
    )


def find_qemu() -> Path:
    """qemu-system-x86_64.exe 的路径。

    候选顺序里显式列出了 MSYS2 的几个前缀（ucrt64 / mingw64 / clang64）：
    MSYS2 装出来的 QEMU 不会自动进 Windows 的 PATH，
    而让用户为了构建去改系统 PATH 是不必要的负担——
    我们只找它，不需要它在那儿。"""
    p = _first_file([
        TOOLCHAIN / "qemu" / "qemu-system-x86_64.exe",
        _search_dirs(["qemu-system-x86_64.exe"]),
        Path(r"C:\Program Files\qemu\qemu-system-x86_64.exe"),
        Path(r"D:\msys64\ucrt64\bin\qemu-system-x86_64.exe"),
        Path(r"C:\msys64\ucrt64\bin\qemu-system-x86_64.exe"),
        Path(r"D:\msys64\mingw64\bin\qemu-system-x86_64.exe"),
        Path(r"C:\msys64\mingw64\bin\qemu-system-x86_64.exe"),
    ])
    if not p:
        raise ToolchainError(
            "找不到 qemu-system-x86_64.exe。\n"
            "  修复（任选其一）：\n"
            "    1) python tools/fetch_toolchain.py   （解出便携版到 toolchain/qemu/）\n"
            "    2) winget install SoftwareFreedomConservancy.QEMU\n"
            "    3) MSYS2: pacman -S mingw-w64-ucrt-x86_64-qemu"
        )
    return p


def find_ovmf() -> Path:
    """UEFI 固件（OVMF / edk2）路径，供 QEMU 使用。"""
    qemu = find_qemu()
    # MSYS2 把固件放在 <prefix>/share/qemu 下，独立安装版放在 <qemu>/share 下，
    # 两种布局都要覆盖——否则会出现「QEMU 找到了、固件找不到」的半通状态。
    p = _first_file([
        TOOLCHAIN / "qemu" / "share" / "edk2-x86_64-code.fd",
        qemu.parent / "share" / "edk2-x86_64-code.fd",
        qemu.parent / "share" / "qemu" / "edk2-x86_64-code.fd",
        Path(r"C:\Program Files\qemu\share\edk2-x86_64-code.fd"),
        Path(r"D:\msys64\ucrt64\share\qemu\edk2-x86_64-code.fd"),
        Path(r"C:\msys64\ucrt64\share\qemu\edk2-x86_64-code.fd"),
        Path(r"D:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"),
    ])
    if not p:
        raise ToolchainError(
            "找不到 edk2-x86_64-code.fd（QEMU 的 UEFI 固件）。\n"
            "  MSYS2 上它在 mingw-w64-ucrt-x86_64-qemu 包里，"
            "装完应在 <msys64>\\ucrt64\\share\\qemu\\ 下。"
        )
    return p


def find_vbox_manage() -> Path:
    p = _first_file([
        Path(r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"),
        _search_dirs(["VBoxManage.exe", "VBoxManage"]),
    ])
    if not p:
        raise ToolchainError("找不到 VBoxManage.exe（VirtualBox 未安装？）")
    return p


# --------------------------------------------------------------------------- #
# 命令执行
# --------------------------------------------------------------------------- #

def run(cmd: Sequence[Path | str], cwd: Path | None = None, quiet: bool = False,
        check: bool = True) -> subprocess.CompletedProcess:
    """执行命令；失败时把完整命令行和输出一起抛出来，便于定位。"""
    argv = [str(c) for c in cmd]
    if not quiet:
        print("  $ " + " ".join(_quote(a) for a in argv), flush=True)
    proc = subprocess.run(argv, cwd=str(cwd or ROOT), capture_output=True, text=True,
                          errors="replace")
    if proc.stdout and not quiet:
        sys.stdout.write(proc.stdout)
    if proc.stderr:
        sys.stderr.write(proc.stderr)
    if check and proc.returncode != 0:
        raise ToolchainError(
            "命令执行失败 (exit {}):\n  {}\n--- stderr ---\n{}".format(
                proc.returncode, " ".join(argv), proc.stderr.strip()))
    return proc


def _quote(s: str) -> str:
    return f'"{s}"' if " " in s else s


def ensure_dirs(*paths: Path) -> None:
    for p in paths:
        p.mkdir(parents=True, exist_ok=True)


def host_path(p: Path) -> str:
    """给子进程用的本机路径字符串。"""
    return str(p)
