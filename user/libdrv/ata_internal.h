/* SPDX-License-Identifier: 0BSD */
/* ata.c 的私有常量。
 *
 * 为什么单独一个头：这些是**ATA 协议自身的数字**（寄存器偏移、状态位、
 * 命令码、PRDT 表项布局），不是 blkd 与 libdrv 之间的接口。
 * 放进 fe_drv.h 会让所有链 libdrv 的服务都看见一堆与它们无关的宏。 */
#ifndef FE_DRV_ATA_INTERNAL_H
#define FE_DRV_ATA_INTERNAL_H

#include <fe_user.h>

/* 命令块寄存器偏移（0x1F0/0x170 起） */
#define ATA_R_DATA    0
#define ATA_R_ERROR   1
#define ATA_R_FEATURE 1
#define ATA_R_SECCNT  2
#define ATA_R_LBA0    3
#define ATA_R_LBA1    4
#define ATA_R_LBA2    5
#define ATA_R_DRIVE   6
#define ATA_R_STATUS  7
#define ATA_R_COMMAND 7

/* 控制块（0x3F6/0x376）：bit1 = nIEN（1 = 关中断），bit2 = 软复位 */
#define ATA_C_DEVCTL  0

/* 状态位 */
#define ATA_ST_BSY  0x80
#define ATA_ST_DRDY 0x40
#define ATA_ST_DF   0x20
#define ATA_ST_DRQ  0x08
#define ATA_ST_ERR  0x01

/* 命令码 */
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_WRITE_PIO    0x30
#define ATA_CMD_READ_DMA     0xC8
#define ATA_CMD_WRITE_DMA    0xCA
#define ATA_CMD_FLUSH_CACHE  0xE7
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_SET_FEATURES 0xEF

#define ATA_FEAT_TRANSFER_MODE 0x03

/* 总线主控寄存器（PCI BAR4 + 通道偏移） */
#define ATA_BM_CMD     0    /* bit0 启动；bit3 方向（1 = 内存 ← 设备） */
#define ATA_BM_STATUS  2    /* bit1 错误；bit2 中断（传输结束） */
#define ATA_BM_PRDT    4    /* 描述符表物理地址 */
#define ATA_BM_CMD_START 0x01
#define ATA_BM_CMD_READ  0x08
#define ATA_BM_ST_ERR    0x02
#define ATA_BM_ST_INTR   0x04

/* PRDT 表项：4 字节物理地址 + 4 字节字节数（bit31 = 最后一项） */
#define ATA_PRD_EOT 0x80000000u

#endif /* FE_DRV_ATA_INTERNAL_H */
