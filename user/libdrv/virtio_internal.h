/* SPDX-License-Identifier: 0BSD */
/* virtio 层内部共用的常量（virtio.c 与将来可能的第二个 virtio 驱动）。
 *
 * 放进私有头而不是 fe_virtio.h：这些是**这一个驱动实现**的布局选择
 * （队列内存怎么切、上限多大），不是"virtio 是什么"。 */
#ifndef FE_DRV_VIRTIO_INTERNAL_H
#define FE_DRV_VIRTIO_INTERNAL_H

#include <fe_user.h>
#include <fe_virtio.h>

/* 驱动支持的最大队列长度。取值理由：这是**驱动侧**的上限，
 * 设备给多大就用多大（可能更小）。256 × 16 字节描述符 = 4 KiB，
 * 正好一页——不是为了好看，是为了让"描述符表自己也不会跨窗口"。 */
#define FE_VIRTQ_MAX_SIZE  256

/* 一块 DMA 对象里放三样东西，各占一页对齐的区段：
 *   0x0000  描述符表（256 × 16 = 4096 字节）
 *   0x1000  available ring（4 + 2×256 = 516 字节）
 *   0x2000  used ring（4 + 8×256 = 2052 字节）
 * 合计 0x3000（12 KiB）。对齐到页是为了让三个结构的物理地址各自
 * 不跨页——那是设备访问它们时最容易踩的边界。 */
#define FE_VIRTQ_TOTAL_BYTES 0x3000
/* 清零/校验的上界（= 上一条的合计），单独给个名字免得 virtio.c 里
 * 出现"看起来像魔法数"的 FE_VIRTQ_MAX_BYTES。 */
#define FE_VIRTQ_MAX_BYTES   FE_VIRTQ_TOTAL_BYTES

#endif /* FE_DRV_VIRTIO_INTERNAL_H */
