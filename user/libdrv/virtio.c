/* SPDX-License-Identifier: 0BSD */
/* virtqueue 机制（virtio 1.0 §2.6）。
 *
 * 这一层只做一件事：**把一段内存按设备能懂的方式排好，并告诉它。**
 * 它不知道块设备是什么——描述符链怎么组、请求头长什么样、状态字节放哪儿，
 * 全是 blkd 的 virtio 后端的事。
 *
 * ★ 三条容易写错、且症状极具误导性的地方，都在这里写清楚 ★
 *
 * 1. **物理地址 vs 虚拟地址是同一块内存的两个视角**
 *    设备只认物理地址（virtq_desc.addr 填的是它），CPU 只认虚拟地址
 *    （我们读写环用的是它）。两者都来自同一个 DMA 内存对象：
 *    fe_mem_alloc_dma 给对象、fe_mem_info 给物理基址、fe_mem_map 给虚拟地址。
 *    把其中任意一个当成另一个用，症状是"设备搬到了别处"——不报错、不崩。
 *
 * 2. **used ring 的下标是 16 位、靠自然溢出回绕**
 *    判"设备完成了没有"必须比较 `used->idx != last_used`，而不是 `<`。
 *    用 `<` 会在第 65536 次请求之后永久卡住——那种"跑了很久突然不动了"
 *    的 bug 几乎不可能靠读代码看出来。
 *
 * 3. **通知（notify）是"写一个 16 位的队列号"到通知地址**
 *    v1.0 之后没有 legacy 那种"写 0x1F0 端口"的东西；通知地址来自
 *    NOTIFY_CFG capability 的 offset，可能还要乘一个 multiplier
 *    （multiplier 存在 capability 的第 16..19 字节）。
 *    漏掉 multiplier 的驱动在 QEMU 上**能跑**（它的 multiplier 是 4 而
 *    偏移是 0），到了真设备上就写错地方——所以这两个值都要读。
 */
#include <fe_drv.h>
#include "virtio_internal.h"


void fe_virtq_init(struct fe_virtq *q, u16 want_size,
                   void *dma_obj_virt, u64 dma_obj_phys)
{
    if (!q) {
        return;
    }
    /* 队列长度必须是 2 的幂：环的下标按位与回绕，不是 2 的幂就会跳格。
     * 上限取 256：更大的队列在这个规模下只是浪费内存与初始化时间
     * （真实好处是"一次挂更多请求"，而本驱动一次只挂一条链）。 */
    if (want_size < 2) {
        want_size = 2;
    }
    if (want_size > 256) {
        want_size = 256;
    }
    q->size = want_size;
    q->last_used = 0;
    q->next_desc = 0;

    q->desc = (struct virtq_desc *)(void *)((u8 *)dma_obj_virt + FE_VIRTQ_OFF_DESC);
    q->avail = (struct virtq_avail *)(void *)((u8 *)dma_obj_virt + FE_VIRTQ_OFF_AVAIL);
    q->used = (struct virtq_used *)(void *)((u8 *)dma_obj_virt + FE_VIRTQ_OFF_USED);
    q->desc_phys = dma_obj_phys + FE_VIRTQ_OFF_DESC;
    q->avail_phys = dma_obj_phys + FE_VIRTQ_OFF_AVAIL;
    q->used_phys = dma_obj_phys + FE_VIRTQ_OFF_USED;

    /* ★ 环必须清零 ★
     * 分配出来的 DMA 内存里可能有上一轮的垃圾（对象来自帧池），
     * 而 avail/used 的 idx 就是"进度"本身——不归零会让设备以为
     * 已经有很多请求待处理，或者让驱动以为设备早就完成了。
     * 清零的范围按**最大队列长度**算，因为此刻还不知道设备会给多大。 */
    for (u32 i = 0; i < FE_VIRTQ_MAX_BYTES; i++) {
        ((u8 *)dma_obj_virt)[i] = 0;
    }
}

int fe_virtq_check(const struct fe_virtq *q)
{
    if (!q || !q->desc || !q->avail || !q->used || q->size == 0) {
        return FE_ERR_INVAL;
    }
    /* 三个结构都必须落在同一个 64 KiB 窗口里：设备按物理地址线性访问，
     * 跨界时它内部的地址加法会失真（与本项目 ATA 那一层的约束同源）。 */
    u64 lo = q->desc_phys & ~0xFFFFull;
    if ((q->avail_phys & ~0xFFFFull) != lo || (q->used_phys & ~0xFFFFull) != lo) {
        return FE_ERR_RANGE;
    }
    u64 hi = q->used_phys + sizeof(struct virtq_used) +
             (u64)q->size * sizeof(struct virtq_used_elem);
    if ((hi & ~0xFFFFull) != lo) {
        return FE_ERR_RANGE;
    }
    return FE_OK;
}
