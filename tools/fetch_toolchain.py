# SPDX-License-Identifier: 0BSD
"""下载并解出 FEKernel 所需的便携工具链（不写注册表、不需要管理员权限）。

用法：
    python tools/fetch_toolchain.py                # 全部下载并解包
    python tools/fetch_toolchain.py --only llvm    # 只处理某一项 (llvm/qemu/limine)
    python tools/fetch_toolchain.py --force        # 强制重新下载

产出目录：
    toolchain/llvm/bin/clang.exe        交叉编译器（x86_64-unknown-none-elf）
    toolchain/llvm/bin/ld.lld.exe       ELF 链接器
    toolchain/llvm/bin/llvm-objcopy.exe 目标文件转换
    toolchain/qemu/qemu-system-x86_64.exe
    toolchain/qemu/share/edk2-x86_64-code.fd   UEFI 固件
    toolchain/limine/BOOTX64.EFI        Limine 引导器（UEFI）

下载源策略：优先国内镜像（清华 TUNA 的 github-release 镜像），失败自动回退官方源。
断点续传 + 速度过低自动重连，避免大文件下到一半卡死。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLCHAIN = ROOT / "toolchain"
DOWNLOADS = TOOLCHAIN / "downloads"

CURL = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "curl.exe"
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")
TAR = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "tar.exe"

LLVM_VERSION = "23.1.2"
LLVM_ASSET = f"clang+llvm-{LLVM_VERSION}-x86_64-pc-windows-msvc.tar.zst"
LIMINE_VERSION = "12.9.0"
QEMU_INSTALLER = "qemu-w64-setup-20260811.exe"


class Artifact:
    def __init__(self, key: str, filename: str, urls: list[str], expect_bytes: int,
                 kind: str, produce: Path):
        self.key = key
        self.filename = filename
        self.urls = urls
        self.expect_bytes = expect_bytes
        self.kind = kind          # "tar.zst" | "7z-nsis" | "7z-zip"
        self.produce = produce    # 解包后应当存在的标志文件


ARTIFACTS = [
    Artifact(
        key="llvm",
        filename=LLVM_ASSET,
        urls=[
            "https://mirrors.tuna.tsinghua.edu.cn/github-release/llvm/llvm-project/"
            "LatestRelease/clang%2Bllvm-" + LLVM_VERSION + "-x86_64-pc-windows-msvc.tar.zst",
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-" + LLVM_VERSION
            + "/" + LLVM_ASSET.replace("+", "%2B"),
        ],
        expect_bytes=276469918,
        kind="tar.zst",
        produce=TOOLCHAIN / "llvm" / "bin" / "clang.exe",
    ),
    Artifact(
        key="qemu",
        filename=QEMU_INSTALLER,
        urls=["https://qemu.weilnetz.de/w64/2026/" + QEMU_INSTALLER],
        expect_bytes=0,
        kind="7z-nsis",
        produce=TOOLCHAIN / "qemu" / "qemu-system-x86_64.exe",
    ),
    Artifact(
        key="limine",
        filename="limine-binary.zip",
        urls=[
            f"https://mirrors.tuna.tsinghua.edu.cn/github-release/Limine-Bootloader/Limine/"
            f"LatestRelease/limine-binary.zip",
            f"https://github.com/Limine-Bootloader/Limine/releases/download/v{LIMINE_VERSION}/"
            f"limine-binary.zip",
        ],
        expect_bytes=2020779,
        kind="7z-zip",
        produce=TOOLCHAIN / "limine" / "BOOTX64.EFI",
    ),
]


def log(msg: str) -> None:
    print(msg, flush=True)


def download(art: Artifact, force: bool = False) -> Path:
    dest = DOWNLOADS / art.filename
    DOWNLOADS.mkdir(parents=True, exist_ok=True)

    if force and dest.exists():
        dest.unlink()

    if dest.exists() and art.expect_bytes and dest.stat().st_size == art.expect_bytes:
        log(f"[{art.key}] 已下载完成，跳过 ({dest.stat().st_size} 字节)")
        return dest

    for url in art.urls:
        log(f"[{art.key}] 下载源: {url}")
        # --speed-limit/--speed-time: 速度低于 4KiB/s 持续 30 秒即断开重连（WARP 下常见）
        cmd = [
            str(CURL), "-L", "--fail", "--retry", "20", "--retry-delay", "3",
            "--retry-all-errors", "-C", "-", "--speed-limit", "4096", "--speed-time", "30",
            "--connect-timeout", "20", "-o", str(dest), url,
        ]
        try:
            subprocess.run(cmd, check=False, timeout=7200)
        except subprocess.TimeoutExpired:
            log(f"[{art.key}] 下载超时，换下一个源")
            continue
        if dest.exists() and dest.stat().st_size > 0:
            size = dest.stat().st_size
            if art.expect_bytes and size != art.expect_bytes:
                log(f"[{art.key}] 大小不符（{size} / 期望 {art.expect_bytes}），换源续传")
                continue
            log(f"[{art.key}] 下载完成: {size} 字节")
            return dest
    raise RuntimeError(f"[{art.key}] 所有下载源均失败")


def extract(art: Artifact, archive: Path) -> None:
    if art.produce.exists():
        log(f"[{art.key}] 已解包，跳过")
        return

    staging = TOOLCHAIN / f"_{art.key}_staging"
    if staging.exists():
        shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True, exist_ok=True)

    log(f"[{art.key}] 解包 -> {staging}")
    if art.kind == "tar.zst":
        # 注意：7z 一次只解开一层压缩。.tar.zst 需要两趟：zst -> tar，tar -> 文件树。
        # （Windows 自带 bsdtar 虽然支持 zstd，但对 LLVM 这种大包会报
        #   "Frame requires too much memory for decoding"，故不用它。）
        subprocess.run([str(SEVENZIP), "x", "-y", f"-o{staging}", str(archive)],
                       check=True, capture_output=True)
        tars = [p for p in staging.iterdir() if p.suffix == ".tar"]
        if not tars:
            raise RuntimeError(f"[{art.key}] 第一层解包后没有找到 .tar（目录内容: "
                               f"{[p.name for p in staging.iterdir()][:5]}）")
        for t in tars:
            log(f"[{art.key}] 第二层解包 {t.name}")
            subprocess.run([str(SEVENZIP), "x", "-y", f"-o{staging}", str(t)],
                           check=True, capture_output=True)
            t.unlink()
    else:
        subprocess.run([str(SEVENZIP), "x", "-y", f"-o{staging}", str(archive)],
                       check=True, capture_output=True)

    # tar 包内部通常有一层顶层目录（clang+llvm-.../），把它提升上来
    entries = list(staging.iterdir())
    if len(entries) == 1 and entries[0].is_dir():
        inner = entries[0]
        target = TOOLCHAIN / art.key
        if target.exists():
            shutil.rmtree(target, ignore_errors=True)
        inner.rename(target)
        shutil.rmtree(staging, ignore_errors=True)
    else:
        target = TOOLCHAIN / art.key
        if target.exists():
            shutil.rmtree(target, ignore_errors=True)
        staging.rename(target)

    if not art.produce.exists():
        # Limine 的 zip 解出来可能带一层 limine-binary/ 目录
        for cand in (TOOLCHAIN / art.key).rglob(art.produce.name):
            log(f"[{art.key}] 标志文件实际位于 {cand}")
            break
        raise RuntimeError(f"[{art.key}] 解包后未找到 {art.produce}")
    log(f"[{art.key}] 就绪: {art.produce}")


def main() -> int:
    ap = argparse.ArgumentParser(description="获取 FEKernel 便携工具链")
    ap.add_argument("--only", choices=[a.key for a in ARTIFACTS], action="append")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if not CURL.is_file():
        log(f"错误: 找不到 curl.exe ({CURL})")
        return 1

    targets = ARTIFACTS if not args.only else [a for a in ARTIFACTS if a.key in args.only]
    for art in targets:
        try:
            archive = download(art, args.force)
            extract(art, archive)
        except Exception as e:  # noqa: BLE001
            log(f"[{art.key}] 失败: {e}")
            return 1
    log("\n全部就绪。可执行: python tools/build.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
