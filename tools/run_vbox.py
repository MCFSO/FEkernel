# SPDX-License-Identifier: 0BSD
"""在 VirtualBox 中启动 FEKernel（headless 跑 + 串口日志 + 屏幕截图）。

用法：
    python tools/run_vbox.py                 # 启动并保持运行（GUI 窗口）
    python tools/run_vbox.py --headless 15   # 无窗口跑 15 秒，打印串口日志并截图
    python tools/run_vbox.py --reset         # 先删除同名虚拟机再重建

为什么 VirtualBox 需要磁盘镜像而不是目录：
    VirtualBox 的固件只认块设备，没有 QEMU vvfat 那种「把目录当 FAT 盘」的功能。
    因此这里先用 tools/mkfat.py 生成原始 FAT32 磁盘镜像，再转成 VDI 挂载。
    好处是这块镜像同时也是后续块设备驱动与 FAT32 服务的测试盘。
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402
import build as febuild  # noqa: E402

ROOT = tc.ROOT
BUILD = tc.BUILD
ESP = BUILD / "esp"
RAW_IMG = BUILD / "disk.img"
VDI = BUILD / "disk.vdi"
SERIAL_LOG = BUILD / "vbox-serial.log"
SCREENSHOT = BUILD / "vbox-screen.png"
VM_NAME = "FEKernel"


def vbox(*args: str, check: bool = True, quiet: bool = True) -> subprocess.CompletedProcess:
    cmd = [str(tc.find_vbox_manage()), *args]
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if proc.returncode != 0 and check:
        raise tc.ToolchainError(
            f"VBoxManage {' '.join(args)} 失败 (exit {proc.returncode}):\n"
            f"{proc.stdout}\n{proc.stderr}")
    if not quiet and proc.stdout.strip():
        print("  " + proc.stdout.strip())
    return proc


def vm_exists() -> bool:
    out = vbox("list", "vms").stdout
    return f'"{VM_NAME}"' in out


def vm_running() -> bool:
    out = vbox("list", "runningvms").stdout
    return f'"{VM_NAME}"' in out


def destroy() -> None:
    if vm_running():
        vbox("controlvm", VM_NAME, "poweroff", check=False)
        time.sleep(2)
    if vm_exists():
        vbox("unregistervm", VM_NAME, "--delete", check=False)
        print(f"[vbox] 已删除旧虚拟机 {VM_NAME}")


def detach_old_disk() -> None:
    """重建镜像前必须先把旧 VDI 从虚拟机与介质注册表里摘干净。
    否则新 VDI 的 UUID 与注册表记录不一致，VirtualBox 会拒绝启动。

    ★ 摘不干净时要**自己把文件删掉**，不能让 convertfromraw 去撞 ★
    实测踩到过：上一次运行留下一个仍然注册在介质表里的 disk.vdi 会锁住文件，
    `closemedium --delete` 报 "locked for writing by another task"，
    紧接着 `convertfromraw` 报 `VERR_ALREADY_EXISTS`，
    而报错信息里只字不提"有台旧虚拟机还在跑"——那是真正的原因。
    所以这里在摘除之后**校验文件是否真的没了**，没没就强删；
    删不掉就把原因说清楚（哪个虚拟机在占用），而不是留给下一步去撞墙。"""
    if vm_running():
        vbox("controlvm", VM_NAME, "poweroff", check=False)
        time.sleep(2)
    if vm_exists():
        for ctl in ("IDE", "SATA"):
            vbox("storageattach", VM_NAME, "--storagectl", ctl, "--port", "0",
                 "--device", "0", "--medium", "none", check=False)
    if VDI.is_file():
        vbox("closemedium", "disk", str(VDI), "--delete", check=False)
    if VDI.is_file():
        # 还锁着：多半是别的虚拟机（或没退干净的进程）占着它
        running = vbox("list", "runningvms").stdout.strip()
        if running:
            print(f"[vbox] 警告：这些虚拟机还在运行，可能占着镜像：\n{running}")
        try:
            VDI.unlink()
            print("[vbox] 旧 VDI 已被强删（介质表里的记录会在下次注册时校正）")
        except OSError as e:
            raise SystemExit(f"[错误] 无法删除 {VDI}：{e}\n"
                             f"        请先关闭占用它的虚拟机（VBoxManage list runningvms）")


def build_image(update: bool = False, reboot: bool = False) -> None:
    """生成镜像并转成 VDI。启动请求通过参数交给 mkfat（见 tools/bootcfg.py）。"""
    args = [sys.executable, str(ROOT / "tools" / "mkfat.py"),
            str(RAW_IMG), str(ESP), "--label", "FEKERNEL"]
    if update:
        args.append("--update-request")
    if reboot:
        args.append("--reboot-request")
    print("[vbox] 生成 FAT32 磁盘镜像")
    subprocess.run(args, check=True, cwd=str(ROOT))
    print("[vbox] 转换为 VDI")
    vbox("convertfromraw", str(RAW_IMG), str(VDI), "--format", "VDI")


def create_vm(cpus: int, memory_mb: int) -> None:
    print(f"[vbox] 创建虚拟机 {VM_NAME}（UEFI 固件, {cpus} CPU, {memory_mb} MiB 内存）")
    vbox("createvm", "--name", VM_NAME, "--ostype", "Other_64", "--register")
    vbox("modifyvm", VM_NAME,
         "--firmware", "efi64",          # 必须用 UEFI：我们只提供 EFI/BOOT/BOOTX64.EFI
         "--memory", str(memory_mb),
         "--cpus", str(cpus),
         "--vram", "32",
         "--ioapic", "on",
         "--rtcuseutc", "on",
         "--boot1", "disk",
         "--boot2", "none",
         "--boot3", "none",
         "--boot4", "none",
         "--audio", "none",
         "--usb", "off",
         "--nic1", "none",
         "--clipboard", "disabled",
         "--draganddrop", "disabled")
    # 串口重定向到文件：内核日志的唯一可靠出口
    SERIAL_LOG.write_bytes(b"")
    vbox("modifyvm", VM_NAME, "--uart1", "0x3F8", "4", "--uartmode1", "file", str(SERIAL_LOG))
    vbox("storagectl", VM_NAME, "--name", "IDE", "--add", "ide",
         "--controller", "PIIX4", "--portcount", "2", "--bootable", "on")
    vbox("storageattach", VM_NAME, "--storagectl", "IDE", "--port", "0", "--device", "0",
         "--type", "hdd", "--medium", str(VDI))


def start(headless: bool) -> None:
    mode = "headless" if headless else "gui"
    print(f"[vbox] 启动虚拟机（{mode}）")
    vbox("startvm", VM_NAME, "--type", mode)
    time.sleep(1)


def stop() -> None:
    if vm_running():
        vbox("controlvm", VM_NAME, "poweroff", check=False)
        time.sleep(1)


def screenshot() -> None:
    if not vm_running():
        return
    r = vbox("controlvm", VM_NAME, "screenshotpng", str(SCREENSHOT), check=False)
    if SCREENSHOT.is_file():
        print(f"[vbox] 屏幕截图 -> {SCREENSHOT}")


def main() -> int:
    ap = argparse.ArgumentParser(description="VirtualBox 启动 FEKernel")
    ap.add_argument("--headless", nargs="?", const=15, type=int, default=0, metavar="秒",
                    help="无窗口运行指定秒数后收集日志并关闭")
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--memory", type=int, default=512)
    ap.add_argument("--reset", action="store_true", help="删除同名虚拟机后重建")
    ap.add_argument("--no-rebuild", action="store_true",
                    help="复用已有镜像与虚拟机。"
                         "★ 这也是看 A/B 第二次启动的方式 ★ —— "
                         "引导控制块在盘上，重建镜像会把它抹回槽 A。")
    ap.add_argument("--ab-update", action="store_true",
                    help="在本次启动的 cmdline 里加上 fek.update=1："
                         "init 拉起更新器，把当前槽推到另一个槽、翻引导控制块并重启。")
    ap.add_argument("--ab-reboot", action="store_true",
                    help="在本次启动的 cmdline 里加上 fek.reboot=1：init 一启动就重启"
                         "（单独验证复位路径）。")
    ap.add_argument("--keys", default="", metavar="扫描码",
                    help="启动后向 PS/2 键盘注入扫描码（十六进制，空格分隔）。"
                         "例: --keys \"1e 9e 20 a0\" 表示按下并松开 A、D。"
                         "这是验证键盘驱动的唯一自动化手段：不需要人真的按键。")
    ap.add_argument("--keys-delay", type=float, default=10.0, metavar="秒",
                    help="启动后多少秒注入按键（默认 10）")
    args = ap.parse_args()

    if not (ESP / "slot_a" / "boot" / "kernel.elf").is_file():
        print("[错误] build/esp 不完整，请先运行: python tools/build.py", file=sys.stderr)
        return 1

    try:
        if args.reset:
            destroy()
        if not args.no_rebuild:
            detach_old_disk()
            build_image(args.ab_update, args.ab_reboot)
        elif args.ab_update or args.ab_reboot:
            print("[vbox] 警告：--no-rebuild 时不会重建镜像，"
                  "--ab-update/--ab-reboot 的请求写不进去", file=sys.stderr)
        if not vm_exists():
            create_vm(args.cpus, args.memory)
        elif not args.no_rebuild:
            # 磁盘镜像换了，重新挂载
            vbox("storageattach", VM_NAME, "--storagectl", "IDE", "--port", "0",
                 "--device", "0", "--type", "hdd", "--medium", str(VDI))
        if SERIAL_LOG.is_file():
            SERIAL_LOG.unlink()
        SERIAL_LOG.write_bytes(b"")
        vbox("modifyvm", VM_NAME, "--uart1", "0x3F8", "4", "--uartmode1", "file", str(SERIAL_LOG))

        start(headless=bool(args.headless))

        if args.headless:
            if args.keys:
                delay = max(0.0, min(args.keys_delay, float(args.headless) - 2.0))
                time.sleep(delay)
                if vm_running():
                    codes = args.keys.split()
                    print(f"[vbox] 向 PS/2 键盘注入扫描码: {' '.join(codes)}"
                          f"（启动后 {delay:.0f}s）")
                    # 一次注入多个：VirtualBox 会按顺序把它们喂给 8042，
                    # 中间不插入人为间隔，正好检验驱动的读取逻辑。
                    vbox("controlvm", VM_NAME, "keyboardputscancode", *codes, check=False)
                time.sleep(max(0.0, float(args.headless) - delay))
            else:
                time.sleep(args.headless)
            screenshot()
            stop()
            print("=" * 70)
            if SERIAL_LOG.is_file():
                print(SERIAL_LOG.read_text(encoding="utf-8", errors="replace"))
            print("=" * 70)
        return 0
    except tc.ToolchainError as e:
        print(f"[错误] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
