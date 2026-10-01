/* SPDX-License-Identifier: 0BSD */
/* virtio 1.0 设备的 **PCI 访问层**：capability 解析 + common cfg 读改写。
 *
 * ★ 为什么这一层必须存在，而不能"直接按偏移怼" ★
 *
 * virtio 1.0 把设备寄存器拆成了**四类结构**（common / notify / isr /
 * device），每一类在哪个 BAR 的哪个偏移、有多长，全部由 PCI capability
 * 描述（规范 §4.1.4）。也就是说：**没有任何一个偏移是固定的**。
 *
 * 这和传统设备（IDE 的 0x1F0、8042 的 0x60）是根本不同的世界：
 * 那一代把地址写死在规范里，这一代把它写在自己的配置空间里。
 * 所以驱动必须先**走一遍 capability 链**，把那四个位置找出来——
 * 这正是 docs/12 说的"设备地址从哪来"在 virtio 上的具体形态。
 *
 * ★ 本文件里没有硬编码的 MMIO 偏移 ★
 * 唯一的固定值是 PCI 配置空间自己的寄存器偏移（那是 PCI 规范的东西，
 * 与 virtio 无关），以及 capability 结构里的字段位置（规范 §4.1.4）。
 */
#include <fe_drv.h>
#include <fe_pci.h>             /* 配置空间访问：capability 链要走它 */
#include "virtio_internal.h"


/* ------------------------------------------------------------------ */
/* PCI capability 链                                                   */
/* ------------------------------------------------------------------ */

/* 走一遍 capability 链，把 virtio 的四类结构位置填进 region[]。
 * region[type] = { bar, offset, length, id }；没找到的 length 为 0。
 *
 * ★ 链表遍历必须设上界 ★
 * capability 的 next 是一个**设备给的字节偏移**。设备（或模拟它的东西）
 * 一旦给出环状的 next，不设上界的遍历就是死循环——而它发生在驱动初始化
 * 阶段，症状是"系统起来就不动了"，与"设备没找到"完全不像。
 * 规范里链最长 48 字节，这里给 64 次迭代的余量。 */
int fe_virtio_pci_scan_caps(u8 bus, u8 dev, u8 func,
                            struct fe_virtio_cap *out, u32 max_type)
{
    if (!out || max_type == 0) {
        return FE_ERR_INVAL;
    }
    for (u32 i = 0; i < max_type; i++) {
        out[i].bar = 0;
        out[i].offset = 0;
        out[i].length = 0;
        out[i].id = 0;
        out[i].found = false;
    }
    /* 状态寄存器的 bit4 = 有 capability 链表；头指针在 0x34 */
    u16 status = fe_pci_cfg_read16(bus, dev, func, FE_PCI_REG_STATUS);
    if (!(status & 0x10)) {
        return FE_ERR_NOENT;        /* 老设备：没有 capability 链 */
    }
    u8 cap = (u8)(fe_pci_cfg_read32(bus, dev, func, 0x34) & 0xFF);
    u32 rounds = 0;
    while (cap && rounds++ < 64) {
        u32 hdr = fe_pci_cfg_read32(bus, dev, func, cap);
        u8 cap_id = (u8)(hdr & 0xFF);
        u8 next = (u8)((hdr >> 8) & 0xFF);
        if (cap_id == VIRTIO_PCI_CAP_VNDR) {
            /* ★ 字段位置严格按规范 §4.1.4 ★
             *   偏移 0  cap_vndr（= 0x09）
             *   偏移 1  cap_next
             *   偏移 2  cap_len（= 20）
             *   偏移 3  cfg_type   ← 注意：它**不在** 4 字节边界上
             *   偏移 4  bar
             *   偏移 5  id
             *   偏移 6  padding[2]
             *   偏移 8  offset（u32，BAR 内字节偏移）
             *   偏移 12 length（u32）
             *   偏移 16 notify_off_multiplier（u32，只有 NOTIFY 有）
             *
             * cfg_type 落在 3 这个"歪"位置，所以两个 32 位读是这样切的：
             *   w0 = [cap+0..3]  低 8 = cap_vndr，次 8 = next，再次 = len，
             *                    **最高 8 = cfg_type**
             *   w1 = [cap+4..7]  低 8 = bar，次 8 = id，高 16 = padding
             * 我第一版把它当成"3..15 全在 w0 的高 24 位里"，于是 bar 读到了
             * 自己的高字节、offset 读成了垃圾——**协议解析错一个字节，
             * 后面全都对不上**，所以这一段必须按规范逐字节推。 */
            u32 w1 = fe_pci_cfg_read32(bus, dev, func, (u8)(cap + 4));
            u8 type = (u8)((hdr >> 24) & 0xFF);     /* cap+3 */
            u8 bar = (u8)(w1 & 0xFF);               /* cap+4 */
            u8 cid = (u8)((w1 >> 8) & 0xFF);        /* cap+5 */
            u32 off = fe_pci_cfg_read32(bus, dev, func, (u8)(cap + 8));
            u32 len = fe_pci_cfg_read32(bus, dev, func, (u8)(cap + 12));
            if (type < max_type) {
                out[type].bar = bar;
                out[type].offset = off;
                out[type].length = len;
                out[type].id = cid;
                out[type].found = true;
            }
            /* ★ NOTIFY 的 multiplier 在偏移 16 ★
             * 通知地址 = notify 的 offset + queue_notify_off × multiplier。
             * QEMU 的 multiplier 是 4 而队列偏移是 0，所以漏掉它**在 QEMU 上
             * 照样跑**——到了真设备（偏移 != 0）就写到别的地方去了。
             * 这类"在模拟器上对、在真机上错"的假设，正是读规范的意义。 */
            if (type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                out[type].multiplier =
                    fe_pci_cfg_read32(bus, dev, func, (u8)(cap + 16));
            }
        }
        cap = next;
    }
    return out[VIRTIO_PCI_CAP_COMMON_CFG].found ? FE_OK : FE_ERR_NOENT;
}

/* 把 capability 原始头 dump 出来（诊断用）。
 *
 * ★ 为什么需要一个专门的诊断入口 ★
 * capability 解析是"错一个字节后面全错"的那类代码，而它的失败表现是
 * "某个区域的地址是垃圾"——从那个症状反推字段位置几乎不可能。
 * 把设备给的原始字节打出来，就能一眼看出是解析错了还是设备给的就不是
 * 我们以为的东西。调用者可以让它只在失败或 verbose 时输出。 */
void fe_virtio_dump_caps(u8 bus, u8 dev, u8 func, void (*emit)(const char *))
{
    if (!emit) {
        return;
    }
    u16 status = fe_pci_cfg_read16(bus, dev, func, FE_PCI_REG_STATUS);
    if (!(status & 0x10)) {
        emit("        （状态寄存器 bit4 = 0：没有 capability 链）\n");
        return;
    }
    u8 cap = (u8)(fe_pci_cfg_read32(bus, dev, func, 0x34) & 0xFF);
    u32 rounds = 0;
    while (cap && rounds++ < 64) {
        u32 hdr = fe_pci_cfg_read32(bus, dev, func, cap);
        u8 cap_id = (u8)(hdr & 0xFF);
        u8 next = (u8)((hdr >> 8) & 0xFF);
        static const char hexd[] = "0123456789abcdef";
        char line[96];
        int n = 0;
        const char *pfx = "        cap@";
        for (int i = 0; pfx[i] && n < 90; i++) {
            line[n++] = pfx[i];
        }
        line[n++] = hexd[(cap >> 4) & 0xF];
        line[n++] = hexd[cap & 0xF];
        line[n++] = ' ';
        line[n++] = 'i';
        line[n++] = 'd';
        line[n++] = '=';
        line[n++] = hexd[(cap_id >> 4) & 0xF];
        line[n++] = hexd[cap_id & 0xF];
        if (cap_id == VIRTIO_PCI_CAP_VNDR) {
            u32 w1 = fe_pci_cfg_read32(bus, dev, func, (u8)(cap + 4));
            u32 off = fe_pci_cfg_read32(bus, dev, func, (u8)(cap + 8));
            u32 len = fe_pci_cfg_read32(bus, dev, func, (u8)(cap + 12));
            const char *keys[4] = { " type=", " bar=", " off=", " len=" };
            u32 vals[4] = { (hdr >> 24) & 0xFF, w1 & 0xFF, off, len };
            for (int k = 0; k < 4; k++) {
                for (int i = 0; keys[k][i] && n < 92; i++) {
                    line[n++] = keys[k][i];
                }
                /* 十进制（够小，便于与规范对照） */
                char tmp[12];
                int m = 0;
                u32 v = vals[k];
                if (v == 0) {
                    tmp[m++] = '0';
                }
                while (v && m < 11) {
                    tmp[m++] = (char)('0' + (v % 10));
                    v /= 10;
                }
                for (int i = m - 1; i >= 0 && n < 93; i--) {
                    line[n++] = tmp[i];
                }
            }
        }
        line[n++] = '\n';
        line[n] = '\0';
        emit(line);
        cap = next;
    }
}

/* ------------------------------------------------------------------ */
/* common cfg 访问器                                                   */
/* ------------------------------------------------------------------ */

/* 四类结构各自的基址（BAR 内偏移已经算好）。
 * mmio_base 是**第一个内存 BAR 映射进来的虚拟地址**。 */
void fe_virtio_set_regions(struct fe_virtio_dev *d, void *mmio_base)
{
    if (!d || !mmio_base) {
        return;
    }
    d->mmio = (u8 *)mmio_base;
    d->common = d->mmio + d->cap[VIRTIO_PCI_CAP_COMMON_CFG].offset;
    d->notify = d->mmio + d->cap[VIRTIO_PCI_CAP_NOTIFY_CFG].offset;
    d->isr = d->mmio + d->cap[VIRTIO_PCI_CAP_ISR_CFG].offset;
    d->device = d->mmio + d->cap[VIRTIO_PCI_CAP_DEVICE_CFG].offset;
}

void fe_virtio_set_regions_direct(struct fe_virtio_dev *d, void *mmio_base,
                                  u32 off_common, u32 off_notify,
                                  u32 off_isr, u32 off_device,
                                  u32 notify_multiplier)
{
    if (!d || !mmio_base) {
        return;
    }
    d->mmio = (u8 *)mmio_base;
    /* 同时把 cap[] 填上：后面的代码（例如 fe_virtio_dump_caps 的调用点
     * 或诊断输出）会读它，留空的 cap[] 会让"哪个区域在哪"变成两处答案。 */
    for (u32 i = 0; i < 6; i++) {
        d->cap[i].found = false;
        d->cap[i].offset = 0;
        d->cap[i].length = 0;
        d->cap[i].bar = 0;
        d->cap[i].id = 0;
    }
    d->cap[VIRTIO_PCI_CAP_COMMON_CFG].found = true;
    d->cap[VIRTIO_PCI_CAP_COMMON_CFG].offset = off_common;
    if (off_notify) {
        d->cap[VIRTIO_PCI_CAP_NOTIFY_CFG].found = true;
        d->cap[VIRTIO_PCI_CAP_NOTIFY_CFG].offset = off_notify;
        d->cap[VIRTIO_PCI_CAP_NOTIFY_CFG].multiplier = notify_multiplier;
    }
    if (off_isr) {
        d->cap[VIRTIO_PCI_CAP_ISR_CFG].found = true;
        d->cap[VIRTIO_PCI_CAP_ISR_CFG].offset = off_isr;
    }
    if (off_device) {
        d->cap[VIRTIO_PCI_CAP_DEVICE_CFG].found = true;
        d->cap[VIRTIO_PCI_CAP_DEVICE_CFG].offset = off_device;
    }
    d->common = d->mmio + off_common;
    d->notify = d->mmio + off_notify;
    d->isr = d->mmio + off_isr;
    d->device = d->mmio + off_device;
    d->notify_multiplier = notify_multiplier;
}
