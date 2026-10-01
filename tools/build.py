# SPDX-License-Identifier: 0BSD
"""FEKernel 构建编排。

用法：
    python tools/build.py            # 构建内核 + 用户态服务 + 组装可启动目录
    python tools/build.py kernel     # 只构建内核
    python tools/build.py esp        # 只组装 ESP 目录
    python tools/build.py clean      # 清理
    python tools/build.py -v         # 显示每条完整命令行

产物：
    build/kernel.elf                 内核（ELF64，Limine 加载）
    build/kernel.map                 链接映射（调试用）
    build/esp/                       EFI 可启动目录树（QEMU/VBox 直接用）
    build/compile_commands.json      给编辑器的编译数据库
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import toolchain as tc  # noqa: E402
import bootcfg  # noqa: E402

ROOT = tc.ROOT
BUILD = tc.BUILD
OBJ = BUILD / "obj"
ESP = BUILD / "esp"

# --------------------------------------------------------------------------- #
# 编译选项
# --------------------------------------------------------------------------- #

# 内核：位于 top 2GiB，不用红区（中断会踩栈），
# ★ 不用浮点/SIMD——这条是**刻意**的，不是省事 ★
#   `-mgeneral-regs-only` 保证内核一行向量寄存器都不碰，
#   于是中断与系统调用**不需要保存 FPU 状态**（延迟的回报）。
#   对称地，用户态现在**可以**用 SSE/AVX（见 USER_CFLAGS 与 fe/fpu.c）。
#   别把这两行删掉："内核顺手用一下 SIMD"会让中断路径必须保存 832 字节状态。
KERNEL_CFLAGS = [
    "--target=x86_64-unknown-none-elf",
    "-std=c17",
    "-ffreestanding",
    "-nostdlib",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-fno-omit-frame-pointer",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-mno-red-zone",
    "-mcmodel=kernel",
    "-mno-sse",
    "-mno-sse2",
    "-mno-mmx",
    "-mno-80387",
    "-mno-avx",
    "-mgeneral-regs-only",
    "-Wall",
    "-Wextra",
    "-Wshadow",
    "-Wvla",
    "-Wundef",
    "-Wcast-align",
    "-Wno-unused-parameter",
    "-O2",
    "-g",
    "-ffunction-sections",
    "-fdata-sections",
]

# 用户态服务：普通小代码模型，固定加载地址，非 PIE 静态 ELF。
#
# ★ SIMD 已放开 ★ —— 基线是 **SSE2**（x86_64 架构保证一定有），
# 由内核在启动时把 CR0/CR4/XCR0 打开（kernel/arch/x86_64/fpu.c），
# 线程切换时保存/恢复状态。所以这里不再有 -mno-sse / -mgeneral-regs-only。
#
# 为什么基线停在 SSE2 而不是 -mavx2：
#   AVX2 不是 x86_64 的保证项。把它编进**每一个**二进制意味着
#   "在只支持 SSE2 的机器上，连 init 都起不来"。要用 AVX2 的正确做法是
#   **运行期分派**（启动时查 CPUID，把热点函数指针指到 AVX2 版本），
#   那是后续的独立优化项——先把"能不能用 SIMD"这件事解决掉。
#
# -mno-red-zone 保留：用户栈由内核在进程启动时摆放，红区不是问题，
# 但保持与内核一致能少一类"到底谁在用 rsp 下面那 128 字节"的疑问。
USER_CFLAGS = [
    "--target=x86_64-unknown-none-elf",
    "-std=c17",
    "-ffreestanding",
    "-nostdlib",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-mno-red-zone",
    "-mcmodel=small",
    "-msse2",                   # 显式写出基线：x86_64 一定有，且内核已打开
    "-mfpmath=sse",             # 浮点走 SSE，不用 x87 栈
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-O2",
    "-g",
    "-ffunction-sections",
    "-fdata-sections",
]

INCLUDES = [
    "-I", ROOT / "kernel" / "include",
    "-I", ROOT / "kernel",
    "-I", ROOT / "user" / "include",
    # POSIX 头（路线 B，见 docs/10-posix-layer.md）：
    # 放最后是**刻意的**——我们自己的头（fe_user.h 等）优先，
    # 标准名（<stdio.h> 之类）只在前面的目录里找不到时才命中这里。
    # 这样"内核 ABI 头"与"POSIX 兼容头"物理上分开，谁都不会悄悄盖住谁。
    "-I", ROOT / "user" / "include" / "posix",
]

USER_BASE = 0x400000          # 用户程序加载基址
KERNEL_LD = ROOT / "kernel" / "linker.ld"
USER_LD = ROOT / "user" / "linker.ld"


# --------------------------------------------------------------------------- #
# 通用构建动作
# --------------------------------------------------------------------------- #

_compile_db: list[dict] = []


def _obj_for(src: Path, group: str) -> Path:
    """目标文件路径。

    注意：要用「原名 + .o」而不是「换掉扩展名」。否则 syscall.c 与 syscall.asm
    会算出同一个目标文件路径，链接时同一个文件被传两次 → duplicate symbol。
    """
    try:
        rel = src.relative_to(ROOT)
    except ValueError:
        rel = Path(src.name)
    return OBJ / group / (rel.name + ".o") if False else OBJ / group / rel.parent / (rel.name + ".o")


def compile_one(src: Path, group: str, cflags: list, quiet: bool, verbose: bool) -> Path:
    out = _obj_for(src, group)
    tc.ensure_dirs(out.parent)
    cmd = [tc.find_clang(), *cflags, *INCLUDES, "-c", src, "-o", out]
    tc.run(cmd, quiet=(quiet and not verbose))
    _compile_db.append({
        "directory": str(ROOT),
        "file": str(src),
        "arguments": [str(c) for c in cmd],
    })
    return out


def assemble_one(src: Path, group: str, quiet: bool, verbose: bool) -> Path:
    out = _obj_for(src, group)
    tc.ensure_dirs(out.parent)
    fmt = "elf64"
    # reloc-* 两类告警是**故意**的：ISR 桩地址表与入口栈顶都是跨段绝对地址引用，
    # 内核固定链接在高半区且不做重定位，这些重定位正是我们想要的。
    cmd = [tc.find_nasm(), "-f", fmt, "-g", "-F", "dwarf", "-Wall",
           "-w-reloc-rel-dword", "-w-reloc-abs-qword", src, "-o", out]
    tc.run(cmd, quiet=(quiet and not verbose))
    return out


def newer_than(src: Path, dst: Path, extra: list[Path] | None = None) -> bool:
    if not dst.is_file():
        return True
    st = dst.stat().st_mtime
    if src.stat().st_mtime > st:
        return True
    for e in (extra or []):
        if e.is_file() and e.stat().st_mtime > st:
            return True
    return False


def build_group(sources: list[Path], group: str, cflags: list, quiet: bool,
                verbose: bool, extra_deps: list[Path]) -> list[Path]:
    objs = []
    for src in sources:
        obj = _obj_for(src, group)
        if newer_than(src, obj, extra_deps):
            obj = compile_one(src, group, cflags, quiet, verbose)
        objs.append(obj)
    return objs


# --------------------------------------------------------------------------- #
# 内核
# --------------------------------------------------------------------------- #

def report_toolchain() -> None:
    """把实际用到的工具链路径打出来，并在发生 PATH 回退时告警。

    为什么值得专门做这件事：工具链定位是「项目内便携版 → PATH → Program Files」
    三级回退，而回退是**静默**的。机器上装了别的编译器（比如 MSYS2 的 mingw gcc、
    系统里的另一个 clang）之后，一旦项目内那份被人清掉或被挪走，
    构建会悄悄地换成另一个编译器——产出一堆格式不对的目标文件，
    然后在链接阶段报出跟真实原因毫无关系的错误。
    一行提示就能把这种排查从「半小时」压到「一眼」。"""
    clang, lld, nasm = tc.find_clang(), tc.find_ld_lld(), tc.find_nasm()
    local = tc.TOOLCHAIN / "llvm" / "bin"
    print(f"[build] 工具链: clang={clang}")
    print(f"[build]         ld.lld={lld}")
    print(f"[build]         nasm={nasm}")
    if local not in clang.parents and clang.parent != local:
        print("[build] ⚠ clang 不是项目内便携版！可能被别的工具链抢走了。")
        print("[build]   修复: python tools/fetch_toolchain.py")
        print("[build]   注意: 混用不同工具链的目标文件会在链接期报出误导性的错误。")


def build_kernel(quiet=False, verbose=False) -> Path:
    print("[build] 内核")
    csrc = sorted((ROOT / "kernel").rglob("*.c"))
    asm = sorted((ROOT / "kernel").rglob("*.asm"))
    if not csrc and not asm:
        raise tc.ToolchainError("kernel/ 下没有源文件")

    hdrs = list((ROOT / "kernel" / "include").rglob("*.h")) + \
           list((ROOT / "kernel").rglob("*.h"))
    objs = build_group(csrc, "kernel", KERNEL_CFLAGS, quiet, verbose, hdrs + [KERNEL_LD])
    objs += [assemble_one(a, "kernel", quiet, verbose) for a in asm]

    out = BUILD / "kernel.elf"
    tc.ensure_dirs(BUILD)
    cmd = [
        tc.find_ld_lld(),
        "-T", KERNEL_LD,
        "-o", out,
        "--Map=" + str(BUILD / "kernel.map"),
        "-z", "max-page-size=0x1000",
        "--build-id=none",
        "--gc-sections",
        "-nostdlib",
        *objs,
    ]
    tc.run(cmd, quiet=(quiet and not verbose))
    size = out.stat().st_size
    print(f"[build] 内核完成 -> {out} ({size} bytes, {len(objs)} 个目标文件)")
    return out


# --------------------------------------------------------------------------- #
# 用户态服务
# --------------------------------------------------------------------------- #

def find_user_programs() -> dict[str, tuple[list[Path], bool]]:
    """用户态程序的发现与分层。

    目录结构**直接表达分层**，而不是靠注释或约定：

        user/include/   fe_user.h（libfe 的接口）/ fe_drv.h（libdrv 的接口）
        user/libfe/     系统调用封装 + 最小运行库   → 链接进**所有**程序
        user/libdrv/    驱动公共设施（8042、将来的 PCI 配置空间…）
                                                    → 只链接进 servers
        user/servers/   常驻服务：拥有硬件资源、对外提供能力
        user/bin/       一次性程序：工具与测试，不碰硬件

    返回值：名字 → (源文件, 是否服务)。
    服务与普通程序的差别不只是「链不链 libdrv」，还有它们在镜像里的位置
    （/sbin 与 /bin）——这正是 Unix 的既有约定，两边一致就不用再解释一遍。
    """
    progs: dict[str, tuple[list[Path], bool]] = {}
    for sub, is_server in (("servers", True), ("bin", False)):
        d0 = ROOT / "user" / sub
        if not d0.is_dir():
            continue
        for d in sorted(d0.iterdir()):
            if not d.is_dir():
                continue
            srcs = sorted(d.rglob("*.c"))
            if srcs:
                progs[d.name] = (srcs, is_server)
    return progs


def build_user(quiet=False, verbose=False) -> dict[str, Path]:
    progs = find_user_programs()
    libfe = sorted((ROOT / "user" / "libfe").rglob("*.c")) if (ROOT / "user" / "libfe").is_dir() else []
    libdrv = sorted((ROOT / "user" / "libdrv").rglob("*.c")) if (ROOT / "user" / "libdrv").is_dir() else []
    # libposix（路线 B）：POSIX 语义层。与 libfe 分开编，因为两者的边界不同：
    # libfe 是「syscall ABI 的投影」，libposix 是「POSIX 接口的投影」。
    # 混在一个库里，"这个函数到底该不该知道设备语义"就会慢慢没人管得住。
    libposix = sorted((ROOT / "user" / "libposix").rglob("*.c")) \
        if (ROOT / "user" / "libposix").is_dir() else []
    asm = sorted((ROOT / "user").rglob("*.asm"))
    if not progs:
        print("[build] 用户态：暂无程序（跳过）")
        return {}

    servers = [n for n, (_s, is_srv) in progs.items() if is_srv]
    print(f"[build] 用户态: 服务 {servers or '(无)'} / 程序 "
          f"{[n for n, (_s, is_srv) in progs.items() if not is_srv]}")
    hdrs = list((ROOT / "user" / "include").rglob("*.h")) if (ROOT / "user" / "include").is_dir() else []
    shared = build_group(libfe, "user", USER_CFLAGS, quiet, verbose, hdrs + [USER_LD])
    shared_asm = [assemble_one(a, "user", quiet, verbose) for a in asm]
    # libposix 链接进所有程序（放在 libfe 之后：重复符号由先出现的那个胜出，
    # 而 libfe 里的 memcpy/malloc 是经过 SIMD 与自检的那一份，应当优先）。
    posixlib = build_group(libposix, "user", USER_CFLAGS, quiet, verbose, hdrs + [USER_LD])
    # libdrv 单独编一份：只有服务链接它，普通程序里不该出现设备语义。
    drvlib = build_group(libdrv, "user", USER_CFLAGS, quiet, verbose, hdrs + [USER_LD])

    outs: dict[str, Path] = {}
    for name, (srcs, is_server) in progs.items():
        objs = build_group(srcs, f"user/{name}", USER_CFLAGS, quiet, verbose,
                           hdrs + [USER_LD])
        out = BUILD / "user" / f"{name}.elf"
        tc.ensure_dirs(out.parent)
        # ★ 显式声明"必须保留"的用户态入口 ★
        # libfe/libposix 是按目标文件链接的，而 --gc-sections 会把**没有任何节
        # 引用**的编译单元整个丢掉（`--gc-sections` 是按节保留：
        # 一个 .o 里的函数如果没人引用，它就不会被拉进来）。
        # 症状是 "undefined symbol: malloc / free / memmove"——明明实现了，
        # 只是被链接器当成垃圾收走了。给这几个最常用的入口加 --undefined，
        # 它们所在的 .o 就会被拉进来，其余仍然照常回收（二进制不会变大多少）。
        keep = []
        for sym in ("malloc", "free", "memmove", "memset", "write", "read",
                    "open", "close", "posix_spawn", "waitpid", "opendir",
                    "printf", "snprintf", "perror", "strerror"):
            keep += ["--undefined", sym]
        cmd = [
            tc.find_ld_lld(),
            "-T", USER_LD,
            f"--defsym=USER_BASE={USER_BASE:#x}",
            "-o", out,
            "-z", "max-page-size=0x1000",
            "--build-id=none",
            "--gc-sections",
            *keep,
            "-nostdlib",
            *objs, *shared, *shared_asm, *posixlib,
            *(drvlib if is_server else []),
        ]
        tc.run(cmd, quiet=(quiet and not verbose))
        outs[name] = out
        kind = "服务" if is_server else "程序"
        print(f"[build]   [{kind}] {name} -> {out} ({out.stat().st_size} bytes)")
    return outs


# --------------------------------------------------------------------------- #
# 组装 ESP（EFI 可启动目录，QEMU 与 VBox 都能用）
# --------------------------------------------------------------------------- #

def build_esp(kernel: Path | None = None, quiet=False, verbose=False) -> Path:
    print("[build] 组装启动目录")
    kernel = kernel or (BUILD / "kernel.elf")
    if not kernel.is_file():
        raise tc.ToolchainError("kernel.elf 不存在，先执行 build.py kernel")

    limine = tc.find_limine_dir()
    if ESP.exists():
        shutil.rmtree(ESP)
    tc.ensure_dirs(ESP / "EFI" / "BOOT", ESP / "boot")

    # Limine 的 UEFI 引导程序必须在 EFI/BOOT/BOOTX64.EFI
    shutil.copy2(limine / "BOOTX64.EFI", ESP / "EFI" / "BOOT" / "BOOTX64.EFI")
    for extra in ("BOOTIA32.EFI", "limine-bios.sys", "limine-bios-cd.bin",
                  "limine-uefi-cd.bin"):
        if (limine / extra).is_file():
            shutil.copy2(limine / extra, ESP / "EFI" / "BOOT" / extra)

    # ---- A/B 双槽布局 ----
    #
    # 学 Linux：可执行文件用**路径**标识，路径就是内核看到的模块路径。
    # A/B 把这一点再推一步：**路径前面多一个槽前缀**，
    # 于是"正在运行哪一份系统"直接体现在它加载的每一个路径里。
    #
    #   slot_<x>/init        → /slot_<x>/init （内核硬编码 exec /init 的替代，见下）
    #   slot_<x>/bin/<名字>  → 一次性程序
    #   slot_<x>/sbin/<名字> → 常驻服务
    #   slot_<x>/boot/kernel.elf
    #   EFI/BOOT/*           Limine 自己 —— **不属于 A/B**，更新引导器是另一回事
    #   home/                用户数据，**两份系统共享**，不参与 A/B
    #
    # 内核仍然硬编码 exec "/init"，所以每个槽里 init 都在槽根的 `init` 上，
    # 而槽根的路径由 Limine 的 cmdline（slot=a/b）告诉内核。
    server_names = {n for n, (_s, is_srv) in find_user_programs().items() if is_srv}
    elfs = sorted((BUILD / "user").glob("*.elf")) if (BUILD / "user").is_dir() else []
    slots = ("a", "b")
    modpaths: dict[str, list[str]] = {s: [] for s in slots}

    for s in slots:
        root = ESP / f"slot_{s}"
        tc.ensure_dirs(root / "boot", root / "bin", root / "sbin")
        shutil.copy2(kernel, root / "boot" / "kernel.elf")
        for elf in elfs:
            name = elf.stem
            if name == "init":
                shutil.copy2(elf, root / "init")
                modpaths[s].append(f"/slot_{s}/init")
            elif name in server_names:
                shutil.copy2(elf, root / "sbin" / name)
                modpaths[s].append(f"/slot_{s}/sbin/{name}")
            else:
                shutil.copy2(elf, root / "bin" / name)
                modpaths[s].append(f"/slot_{s}/bin/{name}")

    # 用户数据：两份系统共享。本轮是空目录，但它的**存在**本身就是设计的一部分——
    # A/B 只复制系统，用户数据不参与，否则每次更新都要搬数据，"无缝"就没了。
    tc.ensure_dirs(ESP / "home")

    # 每个槽一个**构建号**文件。
    #
    # ★ 为什么两个槽的构建号必须不同 ★
    # A/B 的第一次更新得"有东西可更"。两个槽逐字节相同时，更新器会（正确地）
    # 判定无事可做，于是切换路径永远走不到——而一个永远不触发的路径等于没验证过。
    #
    # 这不是为了演示而伪造的差异：真实设备上这两个槽本来就是不同代的构建
    # （一个在跑，一个是下载好的新版）。把它做出来，比"手动改几个字节再宣称
    # 更新成功"诚实得多——后者的"成功"可能只是因为压根没比较。
    #
    # 槽 A = 新（正在跑的这份），槽 B = 旧（出厂预置的那份）。
    # 一次更新做完之后两边就一样了，此时再请求更新是**幂等**的（什么都不做）。
    slot_gen = {"a": 2, "b": 1}
    for s in slots:
        tc.ensure_dirs(ESP / f"slot_{s}" / "etc")
        (ESP / f"slot_{s}" / "etc" / "build-id").write_text(
            f"FEKernel build {slot_gen[s]} (slot {s.upper()})\n", encoding="utf-8")

    # /etc/protect.list 占位：内容是 mkfat.py 在**建镜像的过程中**回填的
    # （那时才知道布局）。大小必须固定，否则回填会挪动布局、清单自己失效。
    # 4 KiB 是给"每个槽几十段区间"留的余量；装不下时 mkfat 会明确报错而不是静默截断。
    tc.ensure_dirs(ESP / "etc")
    (ESP / "etc" / "protect.list").write_bytes(
        b"# placeholder: mkfat.py fills this in after layout\n".ljust(4096, b" "))

    # limine.conf：两个槽各一个条目，**只有 default_entry 那个数字**决定启动哪个。
    # 切槽因此是"改一个字节"，而那个字节的位置由 mkfat.py 算好写进清单。
    conf = [
        "# 本文件由 tools/build.py 自动生成，请勿手工修改",
        "timeout: 3",
        "interface_branding: FEKernel Bootloader",
        "verbose: no",
        # ↓ 这一行的那个数字是**引导控制块**。更新器只改它，不改别处。
        #
        # ★ 它是 1 起算的 ★ —— 这一点我猜错过一次，代价是"翻控制块翻了个寂寞"：
        # 我按 0 起算写 default_entry: 0，又让更新器在 '0'/'1' 之间切换，
        # 结果 0 和 1 **都**落到第一个条目（0 越界后被当成第一个），
        # 于是更新"成功"了、控制块也写进去了，但每次启动还是槽 A。
        # 症状极具迷惑性：控制块回读确实是 '1'，镜像里也确实是
        # "default_entry: 1"，可启动的还是 A —— 因为 1 就是第一个条目。
        # 所以：'1' = 槽 A（第一个条目），'2' = 槽 B（第二个条目）。
        "default_entry: 1",
        "",
    ]
    for i, s in enumerate(slots):
        conf += [
            f"/FEKernel Slot {s.upper()}",
            "    protocol: limine",
            f"    path: boot():/slot_{s}/boot/kernel.elf",
        ]
        for mp in modpaths[s]:
            conf.append(f"    module_path: boot():{mp}")
        # A/B 清单也作为**模块**加载，于是它落在 ramfs 里。
        # 这不是顺手为之：init 必须在**拉起任何服务之前**登记写保护，
        # 而那时 blkd/fsd 都还没起来——清单只能从 ramfs 读，不能从磁盘读。
        # 顺带一个性质：清单与"启动这份系统所需的一切"同源，
        # 想改它就得换掉这一整份引导映像。
        conf.append("    module_path: boot():/etc/protect.list")
        conf.append(f"    cmdline: slot={s}")
        conf.append("")
    (ESP / "limine.conf").write_text("\n".join(conf), encoding="utf-8")
    print(f"[build] limine.conf 已生成：两个槽，default_entry=1（槽 A，1 起算）")
    print(f"[build]   槽 A 模块: {modpaths['a']}")
    print(f"[build]   槽 B 模块: {modpaths['b']}")

    print(f"[build] 启动目录 -> {ESP}")
    return ESP


def set_update_request(on: bool, reboot: bool = False) -> None:
    """把启动请求写进 build/esp/limine.conf 里**被启动那个条目**的 cmdline。

    ★ 现在只是 bootcfg.set_boot_request 的一层转发 ★
    本函数与 mkfat.py 的 `--update-request/--reboot-request` 是同一个实现：
    启动请求是**一次动作**而不是系统属性，所以它不该有第二个真相来源。
    想加标记的工具要么调这里，要么给 mkfat 传参数，两条路落到同一段代码。"""
    bootcfg.set_boot_request(ESP, on, reboot)


def write_compile_db() -> None:
    if _compile_db:
        (BUILD / "compile_commands.json").write_text(
            json.dumps(_compile_db, indent=1), encoding="utf-8")


# --------------------------------------------------------------------------- #
# 入口
# --------------------------------------------------------------------------- #

def clean() -> None:
    for p in (BUILD,):
        if p.exists():
            shutil.rmtree(p)
            print(f"[clean] 删除 {p}")


def main() -> int:
    ap = argparse.ArgumentParser(description="FEKernel 构建")
    ap.add_argument("target", nargs="?", default="all",
                    choices=["all", "kernel", "user", "esp", "clean"])
    ap.add_argument("-q", "--quiet", action="store_true", help="只打印摘要")
    ap.add_argument("-v", "--verbose", action="store_true", help="打印完整命令行")
    args = ap.parse_args()

    tc.ensure_dirs(BUILD, OBJ)
    try:
        if args.target == "clean":
            clean()
            return 0
        kernel = None
        if args.target in ("all", "kernel"):
            report_toolchain()
            kernel = build_kernel(args.quiet, args.verbose)
        if args.target in ("all", "user"):
            build_user(args.quiet, args.verbose)
        if args.target in ("all", "esp"):
            build_esp(kernel, args.quiet, args.verbose)
        write_compile_db()
    except tc.ToolchainError as e:
        print(f"\n[错误] {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
