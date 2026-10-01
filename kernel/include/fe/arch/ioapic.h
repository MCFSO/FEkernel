/* SPDX-License-Identifier: 0BSD */
/* I/O APIC：把外部设备中断路由到各个 CPU 的 LAPIC。
 *
 * 8259 只能把中断送给 BSP，是 SMP 与 MSI 的障碍。M2 起外部中断统一走 IOAPIC，
 * 8259 仅在 APIC 不可用时作为降级路径保留。
 */
#ifndef FE_IOAPIC_H
#define FE_IOAPIC_H

#include <fe/types.h>

/* 寄存器索引（通过 IOREGSEL/IOWIN 间接访问） */
#define FE_IOAPIC_ID      0x00
#define FE_IOAPIC_VER     0x01
#define FE_IOAPIC_ARB     0x02
#define FE_IOAPIC_REDTBL  0x10

/* 重定向表项低位标志 */
#define FE_IOAPIC_MASKED     (1u << 16)
#define FE_IOAPIC_LEVEL      (1u << 15)   /* 电平触发 */
#define FE_IOAPIC_ACTIVE_LOW (1u << 13)

bool fe_ioapic_init(void);
bool fe_ioapic_present(void);
u32  fe_ioapic_gsi_count(void);

/* 把某个 GSI 路由到指定 CPU（物理目标）上的向量 */
void fe_ioapic_route(u32 gsi, u8 vector, u32 dest_apic_id,
                     bool level_triggered, bool active_low, bool masked);
void fe_ioapic_mask(u32 gsi);
void fe_ioapic_unmask(u32 gsi);

void fe_ioapic_dump(void);

#endif /* FE_IOAPIC_H */
