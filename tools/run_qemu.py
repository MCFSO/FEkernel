# SPDX-License-Identifier: 0BSD
"""在 QEMU 中启动 FEKernel。

用法：
    python tools/run_qemu.py                  # 带窗口启动，串口接到终端
    python tools/run_qemu.py --headless 15    # 无窗口跑 15 秒，然后打印串口日志（自动化验证用）
    python tools/run_qemu.py --accel tcg      # 强制纯软件模拟（对比性能用）
    python tools/run_qemu.py --debug          # 冻结 CPU 等待 gdb 连接（-s -S）
    python tools/run_qemu.py --smp 4

关于加速：QEMU 默认的 TCG 是纯软件模拟（慢），本脚本默认优先使用 WHPX
（Windows Hypervisor Platform），CPU 由硬件虚拟化直通，速度接近原生。
"""

from __future__ import annotations

import argparse
import os
import shutil
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
SERIAL_LOG = BUILD / "serial.log"
# 基准测试用的第二块盘（virtio-blk，只读）。
#
# ★ 为什么不复用系统盘 ★
# 系统盘必须挂在 IDE 上（Limine 要从它启动），而 QEMU 的 IDE 盘
# **不支持 DMA**（实测 IDENTIFY w88=0x203F，UDMA 与多字 DMA 支持位全 0）。
# 把基准数据盘单独挂在 virtio-blk 上，两件事各自干净：
# 引导路径一行不动，DMA 路径有真设备可验。
# 磁盘内容不需要有意义（基准测的是"每秒搬多少字节"，不是"搬到了什么"），
# 所以这里只保证它存在且大小固定。
VDISK = BUILD / "bench.img"
VDISK_BYTES = 64 * 1024 * 1024


def ensure_vdisk() -> None:
    """按需生成基准数据盘（全零的稀疏文件即可）。"""
    if VDISK.is_file() and VDISK.stat().st_size == VDISK_BYTES:
        return
    print(f"[qemu] 生成基准数据盘 {VDISK.name}（{VDISK_BYTES // (1024 * 1024)} MiB，只读）")
    with open(VDISK, "wb") as f:
        # seek 到末尾再写一个字节：NTFS 上会得到一个稀疏文件，
        # 不实际占用 64 MiB，也不需要几十秒的清零。
        f.seek(VDISK_BYTES - 1)
        f.write(b"\0")


def pick_accel(requested: str) -> str:
    if requested != "auto":
        return requested
    # 直接问 QEMU 支持哪些加速器，比试探性启动一个 VM 可靠得多
    qemu = tc.find_qemu()
    probe = subprocess.run([str(qemu), "-accel", "help"],
                           capture_output=True, text=True, timeout=30)
    available = (probe.stdout + probe.stderr).lower()
    if "whpx" in available:
        print("[qemu] 加速器: WHPX（Windows Hypervisor Platform，硬件虚拟化直通）")
        return "whpx"
    print("[qemu] 加速器: TCG（纯软件模拟，速度明显较慢）")
    print("       如需开启加速：以管理员身份执行 "
          "Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform")
    return "tcg"


def ovmf_args() -> list[str]:
    code = tc.find_ovmf()
    args = ["-drive", f"if=pflash,format=raw,readonly=on,file={code}"]
    vars_src = code.parent / "edk2-i386-vars.fd"
    if vars_src.is_file():
        vars_dst = BUILD / "ovmf_vars.fd"
        if not vars_dst.is_file():
            shutil.copy2(vars_src, vars_dst)
        args += ["-drive", f"if=pflash,format=raw,file={vars_dst}"]
    return args


def build_image(update: bool, reboot: bool = False) -> None:
    """生成 FAT32 磁盘镜像（与 VirtualBox 用的是同一份）。

    ★ 启动请求通过**参数**交给 mkfat，而不是先改 esp/limine.conf ★
    mkfat 每次都把请求归一化成"调用者要的样子"，所以谁想加标记就必须说出来。
    这样"上一次实验留下的标记"不可能被继承——它就藏在这里过，
    代价是一次普通的启动偷偷做了 A/B 更新并重启（详见 tools/bootcfg.py）。"""
    args = [sys.executable, str(ROOT / "tools" / "mkfat.py"),
            str(RAW_IMG), str(ESP), "--label", "FEKERNEL"]
    if update:
        args.append("--update-request")
    if reboot:
        args.append("--reboot-request")
    print("[qemu] 生成 FAT32 磁盘镜像")
    subprocess.run(args, check=True, cwd=str(ROOT))


def cpu_args(accel: str) -> list[str]:
    """CPU 型号。

    ★ 这一行不是"顺手加的" ★
    QEMU 在 `-machine pc` 下默认给的是 **qemu64** —— 一个很老的 CPU 型号：
    **没有 XSAVE、没有 AVX**。于是客户机里 CR4.OSXSAVE 置不上，
    内核只能退回 FXSAVE（只有 SSE2），"SIMD 放开"这件事就打了对折。

    我第一次就是这么跑的：日志显示 `[FPU] 仅 SSE（FXSAVE）`，
    用户态 CPUID 报 `AVX=无`——**看起来像内核没做对，其实是虚拟机没给**。
    这类"环境限制伪装成实现缺陷"的坑，前面已经在复位路径上踩过一次。

    用 host 直通宿主的 CPU 特性（WHPX 支持）；TCG 下退回 max。
    内核本身是**运行期探测**的（fpu.c 查 CPUID 再决定 XCR0），
    所以在两种情况下都能正确工作：这正是运行期探测该有的样子。

    ★ 为什么要显式关掉 tsc-deadline ★
    启用 -cpu host 之后，CPUID 里出现了 TSC-deadline（ECX[24]），
    内核于是切到 TSC-deadline 定时器路径——然后**一个节拍都不投递**
    （实测 0 Hz，见 kernel/time/time.c 的启动日志）。
    那条路径在本项目里此前**从未被走到过**（qemu64 没有这个特性位），
    所以这是一个**被暴露出来的老问题**，不是 -cpu host 造成的。
    它和 FPU/SIMD 是两件事，所以这一轮先把它显式掩掉、把 SIMD 做完，
    并把"TSC-deadline 在 WHPX 下不工作"作为**未解决的独立问题**记在文档里。
    掩掉一个特性来绕开 bug 是权宜之计，所以必须写在明处，而不是悄悄加上。"""
    if accel in ("whpx", "kvm"):
        return ["-cpu", "host,-tsc-deadline"]
    return ["-cpu", "max,-tsc-deadline"]


def build_command(args) -> list[str]:
    qemu = tc.find_qemu()
    accel = pick_accel(args.accel)

    cmd = [
        str(qemu),
        "-machine", args.machine,
        "-accel", accel,
        "-m", args.mem,
        "-smp", str(args.smp),
    ]
    cmd += cpu_args(accel)
    if not (args.ab_update or args.ab_reboot):
        # ★ 这两个开关平时有用，A/B 那一次必须让开 ★
        #
        # -no-reboot：客户机复位就直接结束 QEMU 进程（否则会一直重启下去）。
        #   平时这是好事：内核三重故障立刻能看出来，而不是无限重启。
        #   但 A/B 更新的**最后一步就是重启**，而重启之后发生的事
        #   （Limine 读被翻过的 default_entry、启动另一个槽、新槽自报成功）
        #   才是这次要验证的东西——禁掉它等于把验证对象掐掉。
        #
        # -no-shutdown：客户机"关机"时只停模拟、不退出进程。它会连带
        #   影响复位路径：进程不退也没有重启，表现为"日志停在复位那一行"。
        cmd += ["-no-reboot", "-no-shutdown"]
    cmd += ovmf_args()
    # 用**我们自己构建的** disk.img，而不是 QEMU 的 `-drive fat:` 目录合成。
    #
    # 为什么：`fat:` 会让 QEMU 现场合成一个文件系统，而它合成出来的是 FAT16
    # （OEM "MSWIN4.1"、分区类型 0x06、没有卷标），和 mkfat.py 产出的
    # FAT32（类型 0xEF、卷标 FEKERNEL）根本不是同一块盘。
    # 那样两个虚拟化环境看到的磁盘内容不一致，「换环境对照」就失去意义了——
    # 块设备驱动读出来的东西不一样，到底是驱动的问题还是盘的问题就说不清。
    #
    # vvfat 的另一个用处本来是「不用建镜像就能启动」，而我们现在本来就建镜像。
    img_rel = os.path.relpath(RAW_IMG, ROOT).replace("\\", "/")
    cmd += ["-drive", f"file={img_rel},format=raw,if=ide,index=0,media=disk"]

    # 第二块盘：virtio-blk（PCI 设备，支持真正的 DMA）。
    #
    # ★ format=raw + readonly=on 是刻意的 ★
    # readonly 让宿主机也保证这块盘不会被写坏——基准测试只读，
    # 而"能不能写"是 fsd 的事，不该混进性能测量里。
    # 这块盘的控制器（virtio-blk-pci）由 pci 总线上的 1af4:1001 标识，
    # 驱动按 PCI 类/厂商 ID 找它，不依赖"它挂在哪一个 slot"。
    if VDISK.is_file():
        vd_rel = os.path.relpath(VDISK, ROOT).replace("\\", "/")
        cmd += ["-drive", f"file={vd_rel},format=raw,if=none,id=bench0,readonly=on"]
        cmd += ["-device", "virtio-blk-pci,drive=bench0"]

    if args.display == "none":
        cmd += ["-display", "none"]
    elif args.display != "default":
        cmd += ["-display", args.display]

    if args.headless:
        cmd += ["-serial", f"file:{SERIAL_LOG}"]
    elif args.serial == "stdio":
        cmd += ["-serial", "stdio", "-monitor", "none"]
    else:
        cmd += ["-serial", args.serial]

    if args.debug:
        cmd += ["-s", "-S"]
    if args.kvm_clock:
        cmd += ["-rtc", "clock=vm"]
    return cmd


def main() -> int:
    ap = argparse.ArgumentParser(description="QEMU 启动 FEKernel")
    ap.add_argument("--accel", choices=["auto", "whpx", "tcg", "hax"], default="auto")
    ap.add_argument("--machine", default="pc",
                    help="机器类型。默认 pc（i440fx/PIIX3）——**它才有传统 IDE 控制器**。"
                         "q35 只有 AHCI，磁盘会被挂到 AHCI 下面，"
                         "于是 ATA PIO 驱动在 0x1F0 上什么都读不到。"
                         "等块设备换成 AHCI/virtio 之后再考虑切回 q35。")
    ap.add_argument("--display", default=None, help="sdl / gtk / none")
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--serial", default="stdio")
    ap.add_argument("--debug", action="store_true", help="冻结等待 gdb（localhost:1234）")
    ap.add_argument("--headless", nargs="?", const=10, type=int, default=0,
                    metavar="秒", help="无窗口运行指定秒数后退出并打印串口日志")
    ap.add_argument("--kvm-clock", action="store_true", help="强制虚拟时钟（调试计时用）")
    ap.add_argument("--ab-update", action="store_true",
                    help="在本次启动的 cmdline 里加上 fek.update=1："
                         "init 会拉起更新器把当前槽推到另一个槽、翻引导控制块并重启。"
                         "★ 本选项自动关掉 -no-reboot ★ —— A/B 的切换点就是重启，"
                         "把它禁掉等于把要验证的那一步掐掉。")
    ap.add_argument("--ab-reboot", action="store_true",
                    help="在本次启动的 cmdline 里加上 fek.reboot=1：init 一启动就请求重启。"
                         "用于单独验证复位路径（不必先跑完一次更新）。")
    ap.add_argument("--keep-image", action="store_true",
                    help="复用现有的 disk.img，不重新生成。"
                         "★ 这是看 A/B「两次启动」的关键 ★ —— 引导控制块在盘上，"
                         "重建镜像会把它抹回 default_entry=0，"
                         "上一次更新切过去的槽就看不见了。")
    args = ap.parse_args()

    if not (ESP / "slot_a" / "boot" / "kernel.elf").is_file():
        print("[错误] build/esp 不完整，请先运行: python tools/build.py", file=sys.stderr)
        return 1

    if args.headless:
        args.display = "none"
    elif args.display is None:
        args.display = "sdl"

    # **先删掉旧的串口日志**，再去做任何可能失败的事（建镜像、起进程）。
    #
    # 为什么顺序重要：如果建镜像失败，脚本会中止，而旧日志还留在那里。
    # 下一步"读日志"看到的就是**上一次启动**的内容，却以为是这一次的结果——
    # 这种误判我在这轮里犯了三次（最后一次是 mkfat 抛异常被管道吞掉）。
    # 先删掉之后，失败就只剩"没有日志"这一种表现，不会伪装成成功。
    if SERIAL_LOG.is_file():
        SERIAL_LOG.unlink()

    try:
        ensure_vdisk()
        if args.keep_image:
            if not RAW_IMG.is_file():
                print(f"[错误] {RAW_IMG} 不存在，--keep-image 没有可复用的镜像；"
                      f"先不带该选项跑一次", file=sys.stderr)
                return 1
            print("[qemu] --keep-image：复用现有 disk.img（含盘上的引导控制块）")
        else:
            build_image(args.ab_update, args.ab_reboot)
        cmd = build_command(args)
    except tc.ToolchainError as e:
        print(f"[错误] {e}", file=sys.stderr)
        return 1

    print("[qemu] " + " ".join(cmd))

    if args.headless:
        proc = subprocess.Popen(cmd, cwd=str(ROOT), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            proc.wait(timeout=args.headless)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=20)
        if SERIAL_LOG.is_file():
            print("=" * 70)
            print(SERIAL_LOG.read_text(encoding="utf-8", errors="replace"))
            print("=" * 70)
        else:
            out = proc.stdout.read() if proc.stdout else ""
            print("[警告] 没有串口日志。QEMU 输出:\n" + out[-4000:])
        return 0

    return subprocess.call(cmd, cwd=str(ROOT))


if __name__ == "__main__":
    raise SystemExit(main())
