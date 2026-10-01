/* SPDX-License-Identifier: 0BSD */
#include <fe/arch/ioapic.h>
#include <fe/arch/lapic.h>
#include <fe/acpi.h>
#include <fe/mm/vmm.h>
#include <fe/kprintf.h>
#include <fe/string.h>

static volatile u32 *g_ioapic;
static u32 g_gsi_base;
static u32 g_max_entry;      /* 可用的重定向表项数 */
static bool g_present;

FE_INLINE u32 ioapic_read(u32 reg)
{
    g_ioapic[0] = reg;              /* IOREGSEL */
    return g_ioapic[4];             /* IOWIN，偏移 0x10 */
}

FE_INLINE void ioapic_write(u32 reg, u32 value)
{
    g_ioapic[0] = reg;
    g_ioapic[4] = value;
}

bool fe_ioapic_present(void) { return g_present; }
u32  fe_ioapic_gsi_count(void) { return g_max_entry; }

bool fe_ioapic_init(void)
{
    const struct fe_acpi_info *acpi = fe_acpi_info();
    if (!acpi->valid || acpi->ioapic_count == 0) {
        /* 没有 ACPI 信息时退回约定地址 */
        g_gsi_base = 0;
        g_ioapic = (volatile u32 *)fe_vmm_map_mmio(0xFEC00000u, FE_FRAME_SIZE);
    } else {
        g_gsi_base = acpi->ioapics[0].gsi_base;
        g_ioapic = (volatile u32 *)fe_vmm_map_mmio(acpi->ioapics[0].address, FE_FRAME_SIZE);
        g_present = true;
    }
    if (!g_ioapic) {
        fe_kprintf("[IOAPIC] MMIO 映射失败\n");
        g_present = false;
        return false;
    }
    u32 ver = ioapic_read(FE_IOAPIC_VER);
    g_max_entry = ((ver >> 16) & 0xFF) + 1;
    g_present = true;

    /* 上电时全部屏蔽，由各驱动按需打开 */
    for (u32 i = 0; i < g_max_entry; i++) {
        ioapic_write(FE_IOAPIC_REDTBL + i * 2, FE_IOAPIC_MASKED);
        ioapic_write(FE_IOAPIC_REDTBL + i * 2 + 1, 0);
    }
    return true;
}

void fe_ioapic_route(u32 gsi, u8 vector, u32 dest_apic_id,
                     bool level_triggered, bool active_low, bool masked)
{
    if (!g_present || gsi < g_gsi_base) {
        return;
    }
    u32 idx = gsi - g_gsi_base;
    if (idx >= g_max_entry) {
        return;
    }
    u32 low = (u32)vector;
    if (level_triggered) {
        low |= FE_IOAPIC_LEVEL;
    }
    if (active_low) {
        low |= FE_IOAPIC_ACTIVE_LOW;
    }
    if (masked) {
        low |= FE_IOAPIC_MASKED;
    }
    u32 reg = FE_IOAPIC_REDTBL + idx * 2;
    ioapic_write(reg + 1, (dest_apic_id & 0xFFu) << 24);
    ioapic_write(reg, low);
}

void fe_ioapic_mask(u32 gsi)
{
    if (!g_present || gsi < g_gsi_base) {
        return;
    }
    u32 idx = gsi - g_gsi_base;
    if (idx >= g_max_entry) {
        return;
    }
    u32 reg = FE_IOAPIC_REDTBL + idx * 2;
    ioapic_write(reg, ioapic_read(reg) | FE_IOAPIC_MASKED);
}

void fe_ioapic_unmask(u32 gsi)
{
    if (!g_present || gsi < g_gsi_base) {
        return;
    }
    u32 idx = gsi - g_gsi_base;
    if (idx >= g_max_entry) {
        return;
    }
    u32 reg = FE_IOAPIC_REDTBL + idx * 2;
    ioapic_write(reg, ioapic_read(reg) & ~FE_IOAPIC_MASKED);
}

void fe_ioapic_dump(void)
{
    if (!g_present) {
        fe_kprintf("[IOAPIC] 不可用\n");
        return;
    }
    fe_kprintf("[IOAPIC] id=%u, 重定向表 %u 项, GSI 基址 %u\n",
               ioapic_read(FE_IOAPIC_ID) >> 24, g_max_entry, g_gsi_base);
}
