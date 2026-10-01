# SPDX-License-Identifier: 0BSD
"""FAT32 磁盘镜像构建器（自研实现，不依赖任何第三方 mkfs 工具）。

用法：
    python tools/mkfat.py <输出镜像> <源目录> [--size-mb N] [--label NAME]
    python tools/mkfat.py build/disk.img build/esp

产出：一个带 MBR 分区表的原始磁盘镜像，其中第一个分区（类型 0xEF，EFI 系统分区）
是 FAT32 文件系统，源目录的全部内容被递归写入。

为什么需要它：
  * VirtualBox / Hyper-V 不接受「目录形式的 FAT」（那是 QEMU 的 vvfat 专有功能）；
  * 后续的块设备驱动（ATA/AHCI/virtio-blk）与 FAT32 文件系统服务需要一块真实磁盘做测试；
  * 自己写 mkfs 也强制我们把 FAT32 的磁盘布局彻底搞明白。

本文件实现的是微软公开的 FAT32 磁盘格式规范（FAT: General Overview / FAT32 File System
Specification），支持长文件名（LFN）。
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bootcfg  # noqa: E402

SECTOR = 512
# 保留扇区从 32 扩到 64：多出来的 32 个扇区放 **A/B 槽状态**（misc）。
#
# 为什么用保留扇区而不是一个文件、也不是一个分区：
#   - 不用文件：文件内容归文件系统管，FS 状态一乱它跟着坏。
#     "下次启动哪一份系统"这种东西不该依赖文件系统的正确性。
#   - 不用分区：多一个分区就要改分区表，而分区表一动，两个槽的相对位置、
#     保留扇区号全都要跟着重算——收益不抵复杂度。
#   - reserved_secs 本来就是"文件系统永远不会分配"的区域，天然合适。
RESERVED_SECTORS = 64
MISC_FIRST_SECTOR = 32          # 保留扇区里，从第 32 个开始是 misc（前 32 个是引导扇区/FSInfo/备份）
MISC_SECTORS = 32
SLOT_STATE_RECORD = 512         # 一份状态记录占一个扇区；写两份、交替更新
NUM_FATS = 2
PART_START_LBA = 2048            # 1 MiB 对齐

# FAT 类型是由**簇数**决定的，不是由 BPB 里写的字符串决定的：
#   簇数 <  4085  → FAT12
#   簇数 < 65525  → FAT16
#   簇数 >= 65525 → FAT32
# 因此一个合法的 FAT32 卷至少要有 65525 个簇；簇大小按卷大小选取（微软规范的推荐表）。
MIN_FAT32_CLUSTERS = 65525
MIN_FAT16_CLUSTERS = 4085


def choose_cluster_size(volume_bytes: int) -> int:
    """返回每簇扇区数（微软推荐的卷大小 / 簇大小对应表）。"""
    mb = volume_bytes // (1024 * 1024)
    if mb < 260:
        return 1        # 512 B
    if mb < 8 * 1024:
        return 8        # 4 KiB
    if mb < 16 * 1024:
        return 16       # 8 KiB
    if mb < 32 * 1024:
        return 32       # 16 KiB
    return 64           # 32 KiB

ATTR_READ_ONLY = 0x01
ATTR_HIDDEN = 0x02
ATTR_SYSTEM = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F

DIR_ENTRY_SIZE = 32
FREE_CLUSTER = 0x00000000
EOC = 0x0FFFFFFF


# --------------------------------------------------------------------------- #
# 目录项与文件树
# --------------------------------------------------------------------------- #

@dataclass
class Node:
    name: str
    is_dir: bool
    data: bytes = b""
    children: list["Node"] = field(default_factory=list)
    first_cluster: int = 0
    size: int = 0


def build_tree(root: Path) -> Node:
    node = Node(name="", is_dir=True)

    def walk(src: Path, dst: Node) -> None:
        entries = sorted(src.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
        for p in entries:
            if p.is_dir():
                child = Node(name=p.name, is_dir=True)
                dst.children.append(child)
                walk(p, child)
            elif p.is_file():
                child = Node(name=p.name, is_dir=False, data=p.read_bytes())
                dst.children.append(child)

    walk(root, node)
    return node


def total_bytes(node: Node) -> int:
    if not node.is_dir:
        return len(node.data)
    return sum(total_bytes(c) for c in node.children)


# --------------------------------------------------------------------------- #
# 短名与长名（LFN）
# --------------------------------------------------------------------------- #

INVALID_SHORT = set('"*+,/:;<=>?[\\]|')


def needs_lfn(name: str) -> bool:
    """判断文件名能否直接放进 8.3 短名。"""
    if name in (".", ".."):
        return False
    if any(ch in INVALID_SHORT or ord(ch) < 0x20 for ch in name):
        return True
    if name != name.upper():
        return True                      # 含小写字母 → 需要 LFN 保留原始大小写
    if name.startswith(" ") or name.endswith(" "):
        return True
    if "." in name:
        base, _, ext = name.rpartition(".")
    else:
        base, ext = name, ""
    # 含多个点的文件名（如 a.b.c）短名无法表达
    if "." in base:
        return True
    return len(base) > 8 or len(ext) > 3 or len(base) == 0


def short_name_for(name: str, taken: set[str]) -> str:
    """生成合法的 8.3 短名（大写、空格填充到 11 字节），必要时加 ~N 后缀。"""
    upper = name.upper()
    if "." in upper:
        base, _, ext = upper.rpartition(".")
    else:
        base, ext = upper, ""
    base = "".join(ch for ch in base if ch not in INVALID_SHORT and ord(ch) >= 0x20)
    ext = "".join(ch for ch in ext if ch not in INVALID_SHORT and ord(ch) >= 0x20)
    if not base:
        base = "_"

    def compose(b: str, e: str) -> str:
        return f"{b:<8}{e:<3}"

    if not needs_lfn(name) or name in (".", ".."):
        cand = compose(base[:8], ext[:3])
        taken.add(cand)
        return cand

    # 需要短名别名：BASE~N.EXT
    for n in range(1, 1000000):
        suffix = f"~{n}"
        b = base[: 8 - len(suffix)] + suffix
        cand = compose(b, ext[:3])
        if cand not in taken:
            taken.add(cand)
            return cand
    raise RuntimeError(f"无法为 {name} 生成短名")


def lfn_checksum(short11: str) -> int:
    s = 0
    for ch in short11.encode("ascii"):
        s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
    return s


def lfn_entries(name: str, short11: str) -> list[bytes]:
    """构造长文件名目录项链（按磁盘顺序：序号从 1 递增，最后一个带 0x40）。"""
    utf16 = name.encode("utf-16-le")
    chunks = [utf16[i:i + 26] for i in range(0, len(utf16), 26)]
    if not chunks:
        return []
    # 末块需要补 0x0000 终止符与 0xFFFF 填充
    last = chunks[-1]
    pad = 26 - len(last)
    if pad >= 2:
        last = last + b"\x00\x00" + b"\xff" * (pad - 2)
    else:
        last = last + b"\xff" * pad
    chunks[-1] = last

    chk = lfn_checksum(short11)
    out = []
    for idx, chunk in enumerate(chunks):
        seq = idx + 1
        if idx == len(chunks) - 1:
            seq |= 0x40
        entry = bytearray(DIR_ENTRY_SIZE)
        entry[0] = seq
        entry[1:11] = chunk[0:10]
        entry[11] = ATTR_LFN
        entry[12] = 0
        entry[13] = chk
        entry[14:26] = chunk[10:22]
        entry[26:28] = b"\x00\x00"
        entry[28:32] = chunk[22:26]
        out.append(bytes(entry))
    return out


def dot_entries(self_cluster: int, parent_cluster: int) -> bytes:
    def make(nm: str, clus: int) -> bytes:
        e = bytearray(DIR_ENTRY_SIZE)
        e[0:11] = nm.encode("ascii").ljust(11)[:11]
        e[11] = ATTR_DIRECTORY
        struct.pack_into("<H", e, 20, (clus >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 26, clus & 0xFFFF)
        return bytes(e)
    return make(".", self_cluster) + make("..", parent_cluster)


def file_entry(short11: str, attr: int, first_cluster: int, size: int) -> bytes:
    e = bytearray(DIR_ENTRY_SIZE)
    e[0:11] = short11.encode("ascii")
    e[11] = attr
    e[12] = 0                      # NT 保留
    e[13] = 0                      # 创建时间十分之一秒
    struct.pack_into("<H", e, 14, 0)    # 创建时间
    struct.pack_into("<H", e, 16, 0x21)  # 创建日期 = 1980-01-01
    struct.pack_into("<H", e, 18, 0x21)  # 最后访问日期
    struct.pack_into("<H", e, 20, (first_cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", e, 22, 0)    # 写入时间
    struct.pack_into("<H", e, 24, 0x21)  # 写入日期
    struct.pack_into("<H", e, 26, first_cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


# --------------------------------------------------------------------------- #
# FAT32 卷
# --------------------------------------------------------------------------- #

class Fat32Volume:
    def __init__(self, total_sectors: int, label: str = "FEKERNEL") -> None:
        self.total_sectors = total_sectors
        self.label = label[:11].ljust(11)
        self.sec_per_cluster = choose_cluster_size(total_sectors * SECTOR)
        self.reserved = RESERVED_SECTORS
        self.num_fats = NUM_FATS
        self.fat_sectors = self._compute_fat_sectors()
        self.data_start = self.reserved + self.num_fats * self.fat_sectors
        self.cluster_count = (self.total_sectors - self.data_start) // self.sec_per_cluster
        if self.cluster_count < MIN_FAT32_CLUSTERS:
            raise RuntimeError(
                f"FAT32 至少需要 {MIN_FAT32_CLUSTERS} 个簇，当前只有 {self.cluster_count} 个"
                f"（卷 {total_sectors // 2048} MiB，簇 {self.sec_per_cluster * SECTOR} 字节）。"
                f"请把卷扩大到至少 "
                f"{(MIN_FAT32_CLUSTERS * self.sec_per_cluster * SECTOR) // (1024 * 1024) + 4} MiB。")
        self.fat = [FREE_CLUSTER] * (self.cluster_count + 2)
        self.fat[0] = 0x0FFFFFF8        # 介质描述符
        self.fat[1] = 0x0FFFFFFF        # 结束标记
        self.image = bytearray(self.total_sectors * SECTOR)
        self.next_free = 2
        self.taken_short_names: set[str] = set()
        # 分配记录：A/B 的槽区间要从这里算出来。
        # 记 (路径, 起始簇, 簇数)，因为分配是深度优先的，
        # 所以一个子树的簇是**连续**的一段——这正是"整槽块级拷贝"成立的前提。
        self.alloc_log: list[tuple[str, int, int]] = []

    def _compute_fat_sectors(self) -> int:
        # FAT 大小取决于簇数，簇数又取决于 FAT 大小 → 迭代收敛
        guess = 1
        while True:
            data_start = self.reserved + self.num_fats * guess
            clusters = (self.total_sectors - data_start) // self.sec_per_cluster
            needed = math.ceil((clusters + 2) * 4 / SECTOR)
            if needed <= guess:
                return guess
            guess = needed

    # ---------------- 簇分配 ----------------

    def alloc_chain(self, count: int) -> int:
        if self.next_free + count > self.cluster_count + 2:
            raise RuntimeError("FAT32 空间不足")
        first = self.next_free
        for i in range(count):
            cur = self.next_free
            self.next_free += 1
            self.fat[cur] = cur + 1 if i < count - 1 else EOC
        return first

    def clusters_for(self, size: int) -> int:
        if size == 0:
            return 0
        return math.ceil(size / (self.sec_per_cluster * SECTOR))

    def cluster_offset(self, cluster: int) -> int:
        return (self.data_start + (cluster - 2) * self.sec_per_cluster) * SECTOR

    def write_cluster(self, cluster: int, data: bytes) -> None:
        off = self.cluster_offset(cluster)
        self.image[off:off + len(data)] = data

    def write_chain(self, first_cluster: int, data: bytes) -> None:
        cluster_bytes = self.sec_per_cluster * SECTOR
        cur = first_cluster
        pos = 0
        while pos < len(data):
            chunk = data[pos:pos + cluster_bytes]
            self.write_cluster(cur, chunk)
            pos += len(chunk)
            cur = self.fat[cur]
            if cur >= EOC and pos < len(data):
                raise RuntimeError("簇链断裂")

    # ---------------- 目录写入 ----------------

    def add_directory_entries(self, parent_cluster: int, entries: bytes) -> None:
        """把目录项写入父目录（必要时扩展父目录的簇链）。"""
        cluster_bytes = self.sec_per_cluster * SECTOR
        # 收集父目录现有的空闲位置：先把父目录整条链读出来
        chain = []
        cur = parent_cluster
        while True:
            chain.append(cur)
            nxt = self.fat[cur]
            if nxt >= EOC:
                break
            cur = nxt

        used = 0
        for c in chain:
            off = self.cluster_offset(c)
            blk = self.image[off:off + cluster_bytes]
            for i in range(0, cluster_bytes, DIR_ENTRY_SIZE):
                if blk[i] == 0x00:      # 从未使用过 → 之后全部空闲
                    break
                used += DIR_ENTRY_SIZE
            else:
                continue
            break

        need = len(entries)
        total_capacity = len(chain) * cluster_bytes
        if used + need > total_capacity:
            extra = math.ceil((used + need - total_capacity) / cluster_bytes)
            new_first = self.alloc_chain(extra)
            # 把新簇接到链尾
            self.fat[chain[-1]] = new_first
            cur = new_first
            while True:
                chain.append(cur)
                nxt = self.fat[cur]
                if nxt >= EOC:
                    break
                cur = nxt

        # 逐块写入目录项
        remaining = entries
        idx = used // cluster_bytes
        inner = used % cluster_bytes
        while remaining:
            c = chain[idx]
            off = self.cluster_offset(c) + inner
            take = min(len(remaining), cluster_bytes - inner)
            self.image[off:off + take] = remaining[:take]
            remaining = remaining[take:]
            idx += 1
            inner = 0

    def populate(self, root: Node) -> None:
        """把文件树写入卷：先递归分配子目录与文件，再写目录项。"""
        root_cluster = self.alloc_chain(1)
        self.root_cluster = root_cluster
        self._populate_dir(root, root_cluster, root_cluster, "")

    # ---------------- A/B 槽区间与 misc ----------------

    def cluster_to_lba(self, cluster: int) -> int:
        """簇号 → **分区内**的相对扇区号。加 PART_START_LBA 才是绝对 LBA。"""
        return self.data_start + (cluster - 2) * self.sec_per_cluster

    def subtree_cluster_range(self, prefix: str) -> tuple[int, int] | None:
        """返回某个子树占用的簇总范围 [lo, hi)（hi 不含）。空子树返回 None。"""
        lo = hi = None
        for (p, first, count) in self.alloc_log:
            if p != prefix and not p.startswith(prefix + "/"):
                continue
            if count == 0:
                continue
            lo = first if lo is None else min(lo, first)
            hi = first + count if hi is None else max(hi, first + count)
        return None if lo is None else (lo, hi)

    def subtree_extents(self, prefix: str) -> list[tuple[int, int]]:
        """返回子树占用的**区间列表**（绝对 LBA, 扇区数），按 LBA 排序。

        ★ 为什么是列表而不是一个连续区间 ★
        我第一版假设"一个子树的簇是连续的"，结果两个槽的区间互相重叠。
        原因是分配顺序：父目录会**先把所有子目录的首簇分配掉**，再逐个递归，
        于是 `/slot_a` 与 `/slot_b` 的目录簇是交错的。
        按区间列表处理就不依赖任何分配顺序的假设——而且这也更接近真实的
        A/B 镜像写入：更新器写的就是一组区间，不是一整块。
        """
        out: list[tuple[int, int]] = []
        for (p, first, count) in self.alloc_log:
            if p != prefix and not p.startswith(prefix + "/"):
                continue
            if count == 0:
                continue
            lba = PART_START_LBA + self.cluster_to_lba(first)
            out.append((lba, count * self.sec_per_cluster))
        out.sort()
        return out

    def subtree_lba_range(self, prefix: str) -> tuple[int, int] | None:
        """子树的整体跨度 [lba, lba+count)（仅供显示；拷贝用 subtree_extents）。"""
        ex = self.subtree_extents(prefix)
        if not ex:
            return None
        lo = ex[0][0]
        hi = max(lba + n for lba, n in ex)
        return (lo, hi - lo)

    def slot_state_bytes(self, seq: int, active: int, successful: int,
                         attempts: int) -> bytes:
        """一份槽状态记录（512 字节，前 16 字节有效，其余填零）。

        两份记录交替写、各带 CRC32：**坏掉一次写入不会让系统失去
        "下次启动哪一份"这个信息**，因为另一份还在。
        这是 Android bootloader_control 的同一套做法。"""
        import zlib
        body = struct.pack("<IIBBBBI", 0x4C534546, seq, active, successful,
                           attempts, 0, 0)
        body = body[:12]              # CRC 覆盖前 12 字节
        crc = zlib.crc32(body) & 0xFFFFFFFF
        rec = body + struct.pack("<I", crc)
        return rec.ljust(SLOT_STATE_RECORD, b"\x00")

    def write_misc(self, seq: int, active: int, successful: int, attempts: int) -> None:
        """把槽状态写进保留扇区。两份都写同样的内容（初始镜像没有历史包袱）。"""
        rec = self.slot_state_bytes(seq, active, successful, attempts)
        for i in range(2):
            off = (MISC_FIRST_SECTOR + i) * SECTOR
            self.image[off:off + SLOT_STATE_RECORD] = rec

    def _populate_dir(self, node: Node, self_cluster: int, parent_cluster: int,
                      path: str = "") -> None:
        # 先为所有子项分配簇
        for child in node.children:
            cpath = f"{path}/{child.name}"
            if child.is_dir:
                # 目录至少一个簇；先用占位内容，随后回填真实目录项
                child.first_cluster = self.alloc_chain(1)
                self.alloc_log.append((cpath, child.first_cluster, 1))
            else:
                n = self.clusters_for(len(child.data))
                child.first_cluster = self.alloc_chain(n) if n else 0
                child.size = len(child.data)
                if n:
                    self.alloc_log.append((cpath, child.first_cluster, n))

        # 递归写入子目录内容（此时子目录自身簇已确定）
        for child in node.children:
            if child.is_dir:
                self._populate_dir(child, child.first_cluster, self_cluster,
                                   f"{path}/{child.name}")

        # 构造本目录的目录项："." 与 ".." 仅非根目录需要
        blob = bytearray()
        if self_cluster != self.root_cluster:
            blob += dot_entries(self_cluster, parent_cluster)

        for child in node.children:
            short11 = short_name_for(child.name, self.taken_short_names)
            if needs_lfn(child.name):
                for e in lfn_entries(child.name, short11):
                    blob += e
            attr = ATTR_DIRECTORY if child.is_dir else ATTR_ARCHIVE
            blob += file_entry(short11, attr, child.first_cluster, child.size)

        # ★ 目录内容超出已分配簇时要**扩容** ★
        #
        # 目录项是 32 字节，一个小写名字要占两项（长名 + 短名）。
        # 512 字节的簇只装得下 8 个这样的文件——加进 protcheck 之后
        # /bin 到了 20 项（640 字节），write_chain 直接报"簇链断裂"。
        #
        # 这个坑一直在，只是以前每个目录都恰好装得下。**能装下不是设计，
        # 是巧合**；写成显式的扩容就把巧合换成了保证。
        cb = self.sec_per_cluster * SECTOR
        need = max(1, math.ceil(len(blob) / cb))
        have = 0
        cur = self_cluster
        while cur and 2 <= cur < len(self.fat) and have < need + 64:
            have += 1
            nxt = self.fat[cur]
            if nxt >= 0x0FFFFFF8 or not (2 <= nxt < len(self.fat)):
                break
            cur = nxt
        if need > have:
            extra = need - have
            new_first = self.alloc_chain(extra)
            # 接到链尾
            tail = self_cluster
            while True:
                nxt = self.fat[tail]
                if nxt >= 0x0FFFFFF8 or not (2 <= nxt < len(self.fat)):
                    break
                tail = nxt
            self.fat[tail] = new_first
            self.alloc_log.append((f"{path}/(目录扩容)", new_first, extra))

        # 写数据
        for child in node.children:
            if not child.is_dir and child.data:
                self.write_chain(child.first_cluster, child.data)
        self.write_chain(self_cluster, bytes(blob))

    # ---------------- 序列化 ----------------

    def boot_sector(self) -> bytes:
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"FEKERNEL"
        struct.pack_into("<H", bs, 11, SECTOR)
        bs[13] = self.sec_per_cluster
        struct.pack_into("<H", bs, 14, self.reserved)
        bs[16] = self.num_fats
        struct.pack_into("<H", bs, 17, 0)          # RootEntCnt（FAT32 必须为 0）
        struct.pack_into("<H", bs, 19, 0)          # TotSec16
        bs[21] = 0xF8                              # Media
        struct.pack_into("<H", bs, 22, 0)          # FATSz16
        struct.pack_into("<H", bs, 24, 63)         # SecPerTrk
        struct.pack_into("<H", bs, 26, 255)        # NumHeads
        struct.pack_into("<I", bs, 28, PART_START_LBA)
        struct.pack_into("<I", bs, 32, self.total_sectors)
        struct.pack_into("<I", bs, 36, self.fat_sectors)
        struct.pack_into("<H", bs, 40, 0)          # ExtFlags：两个 FAT 都有效
        struct.pack_into("<H", bs, 42, 0)          # FSVer
        struct.pack_into("<I", bs, 44, self.root_cluster)
        struct.pack_into("<H", bs, 48, 1)          # FSInfo 扇区号
        struct.pack_into("<H", bs, 50, 6)          # 备份引导扇区号
        bs[64] = 0x80                              # DrvNum
        bs[66] = 0x29                              # 扩展引导签名
        struct.pack_into("<I", bs, 67, 0xFE202409)  # 卷序列号
        bs[71:82] = self.label.encode("ascii")
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        return bytes(bs)

    def fsinfo_sector(self) -> bytes:
        s = bytearray(SECTOR)
        struct.pack_into("<I", s, 0, 0x41615252)   # 引导签名
        struct.pack_into("<I", s, 484, 0x61417272)  # 结构签名
        used = self.next_free - 2
        free = self.cluster_count - used
        struct.pack_into("<I", s, 488, free)
        struct.pack_into("<I", s, 492, self.next_free)
        struct.pack_into("<I", s, 508, 0xAA550000)
        return bytes(s)

    def fat_bytes(self) -> bytes:
        out = bytearray(self.fat_sectors * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<I", out, i * 4, v)
        return bytes(out)

    # ---------------- 构建期回填（占位文件 / 定位控制块字节） ----------------

    def _file_entry(self, path: str) -> tuple[int, int] | None:
        """返回 (起始簇, 簇数)；不是文件或不存在返回 None。"""
        for (p, first, count) in self.alloc_log:
            if p == path and count:
                return (first, count)
        return None

    def file_bytes(self, path: str) -> bytes | None:
        e = self._file_entry(path)
        if e is None:
            return None
        first, count = e
        cb = self.sec_per_cluster * SECTOR
        out = bytearray()
        cur = first
        for _ in range(count):
            off = self.cluster_offset(cur)
            out += self.image[off:off + cb]
            cur = self.fat[cur] if 2 <= self.fat[cur] < len(self.fat) else 0
            if not cur:
                break
        return bytes(out)

    def patch_file(self, path: str, content: bytes) -> bool:
        """把已有文件的内容**原地改写**（不分配、不改大小）。

        只用于构建期回填占位文件：`/etc/protect.list` 必须先以固定大小存在，
        布局定下来才知道该写什么进去；大小固定，所以回填不会挪动任何东西。"""
        e = self._file_entry(path)
        if e is None:
            return False
        first, count = e
        cb = self.sec_per_cluster * SECTOR
        if len(content) > count * cb:
            return False
        blob = content.ljust(count * cb, b" ")
        cur = first
        pos = 0
        for _ in range(count):
            off = self.cluster_offset(cur)
            self.image[off:off + cb] = blob[pos:pos + cb]
            pos += cb
            cur = self.fat[cur] if 2 <= self.fat[cur] < len(self.fat) else 0
            if not cur:
                break
        return True

    def locate_in_file(self, path: str, needle: bytes, skip: int = 0
                       ) -> tuple[int, int] | None:
        """在文件里找到 needle 之后的第 skip 个字节，返回它的
        **(绝对 LBA, 扇区内偏移)**。找不到返回 None。

        有了这个，切换启动槽就是"改一个字节"，而改字节的人
        **完全不需要懂 FAT** —— 位置是构建期算好写进清单的。"""
        data = self.file_bytes(path)
        if data is None:
            return None
        idx = data.find(needle)
        if idx < 0:
            return None
        idx += skip
        if idx >= len(data):
            return None
        cb = self.sec_per_cluster * SECTOR
        clu_idx = idx // cb
        inner = idx % cb
        e = self._file_entry(path)
        first = e[0]
        cur = first
        for _ in range(clu_idx):
            cur = self.fat[cur] if 2 <= self.fat[cur] < len(self.fat) else 0
            if not cur:
                return None
        lba = PART_START_LBA + self.cluster_offset(cur) // SECTOR + inner // SECTOR
        return (lba, inner % SECTOR)

    def serialize(self, partition_offset: int) -> bytes:
        # 引导扇区 + 备份
        bs = self.boot_sector()
        self.image[0:SECTOR] = bs
        self.image[6 * SECTOR:7 * SECTOR] = bs
        self.image[SECTOR:2 * SECTOR] = self.fsinfo_sector()
        self.image[7 * SECTOR:8 * SECTOR] = self.fsinfo_sector()

        fat = self.fat_bytes()
        for i in range(self.num_fats):
            off = (self.reserved + i * self.fat_sectors) * SECTOR
            self.image[off:off + len(fat)] = fat
        return bytes(self.image)


def mbr(volume_sectors: int, total_sectors: int) -> bytes:
    m = bytearray(SECTOR)
    # 分区 1：EFI 系统分区
    e = 446
    m[e + 0] = 0x80                     # 可引导标志（UEFI 不看，但 BIOS 链会看）
    m[e + 1:e + 4] = b"\x00\x02\x00"    # CHS 起始（固件会忽略）
    m[e + 4] = 0xEF                     # 类型：EFI System Partition
    m[e + 5:e + 8] = b"\xFE\xFF\xFF"    # CHS 结束
    struct.pack_into("<I", m, e + 8, PART_START_LBA)
    struct.pack_into("<I", m, e + 12, volume_sectors)
    m[510:512] = b"\x55\xAA"
    return bytes(m)


# --------------------------------------------------------------------------- #
# 独立自检：把生成的镜像当成「别人的镜像」重新解析一遍
# --------------------------------------------------------------------------- #

def verify(image: bytes) -> list[str]:
    problems: list[str] = []

    def bad(msg: str) -> None:
        problems.append(msg)

    # --- MBR ---
    if image[510:512] != b"\x55\xAA":
        bad("MBR 缺少 0x55AA 签名")
    ptype = image[446 + 4]
    part_lba = struct.unpack_from("<I", image, 446 + 8)[0]
    part_len = struct.unpack_from("<I", image, 446 + 12)[0]
    if ptype != 0xEF:
        bad(f"分区类型不是 EFI(0xEF)，而是 {ptype:#x}")
    if part_lba != PART_START_LBA:
        bad(f"分区起始 LBA 异常: {part_lba}")

    # --- 引导扇区 ---
    off = part_lba * SECTOR
    bs = image[off:off + SECTOR]
    if bs[510:512] != b"\x55\xAA":
        bad("引导扇区缺少 0x55AA 签名")
    bps = struct.unpack_from("<H", bs, 11)[0]
    spc = bs[13]
    rsvd = struct.unpack_from("<H", bs, 14)[0]
    nfats = bs[16]
    tot32 = struct.unpack_from("<I", bs, 32)[0]
    fatsz = struct.unpack_from("<I", bs, 36)[0]
    rootclus = struct.unpack_from("<I", bs, 44)[0]
    fsinfo = struct.unpack_from("<H", bs, 48)[0]
    if bps != SECTOR:
        bad(f"每扇区字节数应为 {SECTOR}，实际 {bps}")
    if spc == 0 or (spc & (spc - 1)):
        bad(f"每簇扇区数必须是 2 的幂，实际 {spc}")
    if tot32 != part_len:
        bad(f"TotSec32({tot32}) 与 MBR 分区长度({part_len}) 不一致")
    if bs[82:90] != b"FAT32   ":
        bad(f"文件系统类型字符串异常: {bs[82:90]!r}")

    data_start = rsvd + nfats * fatsz
    clusters = (tot32 - data_start) // spc
    if clusters < MIN_FAT32_CLUSTERS:
        bad(f"簇数 {clusters} < {MIN_FAT32_CLUSTERS}：会被判定成 FAT16 而不是 FAT32")

    # --- FSInfo ---
    fs = image[off + fsinfo * SECTOR: off + (fsinfo + 1) * SECTOR]
    if struct.unpack_from("<I", fs, 0)[0] != 0x41615252:
        bad("FSInfo 引导签名错误")
    if struct.unpack_from("<I", fs, 484)[0] != 0x61417272:
        bad("FSInfo 结构签名错误")
    if struct.unpack_from("<I", fs, 508)[0] != 0xAA550000:
        bad("FSInfo 尾部签名错误")

    # --- 备份引导扇区 ---
    bk = struct.unpack_from("<H", bs, 50)[0]
    if image[off + bk * SECTOR: off + bk * SECTOR + SECTOR] != bs:
        bad(f"备份引导扇区（第 {bk} 扇区）与主引导扇区不一致")

    # --- FAT 表 ---
    def fat_entry(i: int) -> int:
        p = off + rsvd * SECTOR + i * 4
        return struct.unpack_from("<I", image, p)[0] & 0x0FFFFFFF

    if nfats >= 2:
        f0 = image[off + rsvd * SECTOR: off + (rsvd + fatsz) * SECTOR]
        f1 = image[off + (rsvd + fatsz) * SECTOR: off + (rsvd + 2 * fatsz) * SECTOR]
        if f0 != f1:
            bad("两份 FAT 表内容不一致")
    if (fat_entry(0) & 0xFF) != 0xF8:
        bad(f"FAT[0] 介质描述符应为 0xF8，实际 {fat_entry(0) & 0xFF:#x}")

    # --- 目录树遍历 ---
    def cluster_offset(c: int) -> int:
        return off + (data_start + (c - 2) * spc) * SECTOR

    def read_chain(first: int, limit_bytes: int | None = None) -> bytes:
        out = bytearray()
        cur = first
        guard = 0
        while 2 <= cur < clusters + 2:
            out += image[cluster_offset(cur):cluster_offset(cur) + spc * SECTOR]
            nxt = fat_entry(cur)
            if nxt >= 0x0FFFFFF8:
                break
            if nxt < 2 or nxt >= clusters + 2:
                bad(f"簇链出现非法后继：{cur} -> {nxt}")
                break
            cur = nxt
            guard += 1
            if guard > clusters:
                bad("簇链成环")
                break
        if limit_bytes is not None:
            return bytes(out[:limit_bytes])
        return bytes(out)

    visited_dirs = 0

    def walk_dir(cluster: int, path: str, depth: int = 0) -> None:
        nonlocal visited_dirs
        if depth > 16:
            bad(f"目录层级过深: {path}")
            return
        visited_dirs += 1
        data = read_chain(cluster)
        lfn_parts: list[str] = []
        for i in range(0, len(data), DIR_ENTRY_SIZE):
            e = data[i:i + DIR_ENTRY_SIZE]
            if len(e) < DIR_ENTRY_SIZE or e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn_parts = []
                continue
            if e[11] == ATTR_LFN:
                chunk = e[1:11] + e[14:26] + e[28:32]
                lfn_parts.append(chunk.decode("utf-16-le", errors="replace"))
                continue
            if e[11] & ATTR_VOLUME_ID:
                lfn_parts = []
                continue

            short = e[0:11].decode("ascii", errors="replace")
            if short.startswith("."):
                lfn_parts = []
                continue
            clus = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            name = short.strip()
            if lfn_parts:
                # LFN 链是按磁盘顺序存的 1,2,3...，拼接后再反转
                name = "".join(p for p in lfn_parts)
                lfn_parts = []

            full = f"{path}/{name}"
            is_dir = bool(e[11] & ATTR_DIRECTORY)
            if is_dir:
                if not (2 <= clus < clusters + 2):
                    bad(f"目录 {full} 的首簇非法: {clus}")
                else:
                    walk_dir(clus, full, depth + 1)
            else:
                if size > 0 and not (2 <= clus < clusters + 2):
                    bad(f"文件 {full} 的首簇非法: {clus}")
                else:
                    content = read_chain(clus, size) if size else b""
                    if len(content) != size:
                        bad(f"文件 {full} 实际可读 {len(content)} 字节，声明 {size} 字节")

    if not (2 <= rootclus < clusters + 2):
        bad(f"根目录簇号非法: {rootclus}")
    else:
        walk_dir(rootclus, "")

    if visited_dirs == 0:
        bad("没有遍历到任何目录")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description="构建 FAT32 磁盘镜像")
    ap.add_argument("output", type=Path)
    ap.add_argument("source", type=Path)
    ap.add_argument("--size-mb", type=int, default=0, help="卷大小（MiB），默认按内容自动计算")
    ap.add_argument("--label", default="FEKERNEL")
    # ★ 启动请求：由**建镜像的人**显式声明 ★
    # 不给就是"不请求"，并且会把上一次留在 esp/limine.conf 里的标记**擦掉**。
    # 这是为了消灭"实验状态藏在构建产物里"这个坑：
    # 直接调 mkfat 的工具（抓屏、注入鼠标事件）此前会继承上一个实验的标记，
    # 于是一次普通的启动偷偷做了 A/B 更新并重启。详见 tools/bootcfg.py 的说明。
    ap.add_argument("--update-request", action="store_true",
                    help="在被启动条目的 cmdline 上加 fek.update=1（本次启动执行一次 A/B 更新）")
    ap.add_argument("--reboot-request", action="store_true",
                    help="在被启动条目的 cmdline 上加 fek.reboot=1（本次启动立即重启）")
    args = ap.parse_args()

    if not args.source.is_dir():
        print(f"[错误] 源目录不存在: {args.source}", file=sys.stderr)
        return 1

    # 先把启动请求设成调用者要的样子（幂等，会把上一次的标记擦掉）。
    # 必须**在建树之前**：limine.conf 的内容会被写进镜像，
    # 而启动控制块的定位（locate_in_file）是靠在镜像里搜 "default_entry: "，
    # 改长度会影响文件内容——所以先改源文件、再建树，两边天然一致。
    bootcfg.set_boot_request(args.source, args.update_request,
                             args.reboot_request)

    tree = build_tree(args.source)
    payload = total_bytes(tree)
    file_count = sum(1 for _ in args.source.rglob("*") if _.is_file())

    if args.size_mb:
        volume_bytes = args.size_mb * 1024 * 1024
    else:
        # 下限 48 MiB：512 字节簇时刚好满足 FAT32 的 65525 簇下限
        volume_bytes = max(int(payload * 2) + 8 * 1024 * 1024, 48 * 1024 * 1024)
    volume_sectors = volume_bytes // SECTOR
    total_sectors = PART_START_LBA + volume_sectors

    print(f"[mkfat] 内容 {payload} 字节 / {file_count} 个文件")
    print(f"[mkfat] 卷大小 {volume_sectors * SECTOR // 1024 // 1024} MiB")

    try:
        vol = Fat32Volume(volume_sectors, args.label)
    except RuntimeError as e:
        print(f"[错误] {e}", file=sys.stderr)
        return 1
    print(f"[mkfat] 簇 {vol.sec_per_cluster * SECTOR} 字节, 共 {vol.cluster_count} 个 "
          f"(FAT32 要求 >= {MIN_FAT32_CLUSTERS})")
    print(f"[mkfat] FAT 表 {vol.fat_sectors} 扇区/份, 数据区起始扇区 {vol.data_start}")

    vol.populate(tree)

    # ---- A/B：槽状态、槽区间、启动控制块位置 ----
    #
    # 这三样都由构建期算出来，写进 /etc/protect.list 交给 init 与更新器用。
    # 关键好处：**更新器不需要懂 FAT**——切槽就是"改清单里给的那个字节"。
    vol.write_misc(seq=1, active=0, successful=1, attempts=0)

    slot_extents: dict[str, list[tuple[int, int]]] = {}
    for name in ("a", "b"):
        ex = vol.subtree_extents(f"/slot_{name}")
        if ex:
            slot_extents[name] = ex

    bootsel = vol.locate_in_file("/limine.conf", b"default_entry: ",
                                 len(b"default_entry: "))
    if bootsel:
        # 这个字节是 '1'（槽 A）或 '2'（槽 B）。
        # ★ default_entry 是 1 起算的 ★ —— 曾经写成 '0'/'1'，
        # 结果是 0 和 1 都落到第一个条目，切槽切了个寂寞。详见 docs/05 §10。
        lba, off = bootsel
        cur = vol.image[(lba - PART_START_LBA) * SECTOR + off]
        if cur not in (ord("1"), ord("2")):
            print(f"[mkfat] 警告：启动控制块那个字节是 {cur!r}（应为 '1' 或 '2'）",
                  file=sys.stderr)

    lines = [
        "# FEKernel A/B 布局清单 —— 由 tools/mkfat.py 在**建镜像的过程中**生成。",
        "# 描述的就是这一次的布局，两者天然自洽，不存在「两遍构建」的收敛问题。",
        "# 语法：<关键词> <参数...>；'#' 起注释。由 init 读取并逐条执行。",
        "#",
        "# slot <a|b> <lba> <count>  槽的组成区间（可能多条）；更新器按顺序逐段拷贝",
        "# misc <lba> <count>        A/B 槽状态记录所在",
        "# bootsel <lba> <offset>    引导控制块：那个决定启动哪个槽的字节",
        f"misc {PART_START_LBA + MISC_FIRST_SECTOR} {MISC_SECTORS}",
        # 唯一能碰"另一个槽"的进程。写成**槽内相对路径**，由内核补上
        # 当前槽的前缀——因为更新器永远是"正在运行的那份系统"里的那个。
        "updater bin/abupdate",
    ]
    for name in sorted(slot_extents):
        for (lba, count) in slot_extents[name]:
            lines.append(f"slot {name} {lba} {count}")
    if bootsel:
        lines.append(f"bootsel {bootsel[0]} {bootsel[1]}")
    content = ("\n".join(lines) + "\n").encode("utf-8")

    if not vol.patch_file("/etc/protect.list", content):
        ent = vol._file_entry("/etc/protect.list")
        print(f"[mkfat] 警告：/etc/protect.list 占位文件不存在或太小"
              f"（条目={ent}，内容={len(content)} 字节）", file=sys.stderr)
    else:
        print(f"[mkfat] A/B 清单已写入 /etc/protect.list（{len(content)} 字节）")
        for name in sorted(slot_extents):
            ex = slot_extents[name]
            total = sum(n for _l, n in ex)
            print(f"[mkfat]   槽 {name.upper()}: {len(ex)} 段区间，共 {total} 扇区 "
                  f"({total * SECTOR // 1024} KiB)")
        if bootsel:
            print(f"[mkfat]   启动控制块字节: LBA {bootsel[0]} 偏移 {bootsel[1]}")
        if len(slot_extents) == 2:
            a, b = slot_extents["a"], slot_extents["b"]
            if len(a) != len(b):
                print(f"[mkfat] 警告：两个槽的区间段数不同（{len(a)} vs {len(b)}），"
                      f"逐段拷贝将不可用", file=sys.stderr)
            elif any(x[1] != y[1] for x, y in zip(a, b)):
                print("[mkfat] 警告：两个槽对应区间的长度不同，逐段拷贝将不可用",
                      file=sys.stderr)
            else:
                print(f"[mkfat]   两个槽逐段对齐（{len(a)} 段），可直接逐段拷贝")

    volume_image = vol.serialize(PART_START_LBA)

    out = bytearray(total_sectors * SECTOR)
    out[0:SECTOR] = mbr(volume_sectors, total_sectors)
    out[PART_START_LBA * SECTOR:PART_START_LBA * SECTOR + len(volume_image)] = volume_image

    problems = verify(bytes(out))
    if problems:
        print("[mkfat] 自检未通过：", file=sys.stderr)
        for p in problems:
            print(f"        · {p}", file=sys.stderr)
        return 1
    print("[mkfat] 自检通过（重新解析镜像：MBP/BPB/FSInfo/双 FAT/目录树/簇链 全部一致）")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(f"[mkfat] 已生成 {args.output} ({len(out) // 1024 // 1024} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
