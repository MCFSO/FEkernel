/* SPDX-License-Identifier: 0BSD */
#include <fe/acpi.h>
#include <fe/mm/vmm.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/panic.h>

static struct fe_acpi_info g_info;

const struct fe_acpi_info *fe_acpi_info(void)
{
    return &g_info;
}

/* 引导器给出的固件表地址有的实现给物理地址，有的给 HHDM 虚拟地址。
 * 这里先按原值校验签名，不通过再按物理地址（加 HHDM 偏移）试一次。 */
static void *fix_phys_ptr(void *p)
{
    u64 v = (u64)(uptr)p;
    if (v == 0) {
        return NULL;
    }
    u64 hhdm = fe_vmm_hhdm_offset();
    if (hhdm != 0 && v < hhdm) {
        return (void *)(uptr)(v + hhdm);
    }
    return p;
}

static bool checksum_ok(const void *data, u32 length)
{
    const u8 *p = (const u8 *)data;
    u8 sum = 0;
    for (u32 i = 0; i < length; i++) {
        sum = (u8)(sum + p[i]);
    }
    return sum == 0;
}

static bool sig_eq(const char *a, const char *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

/* 在 RSDT/XSDT 里按签名找一张表 */
static const struct fe_acpi_sdt_header *find_table(const struct fe_acpi_rsdp *rsdp,
                                                   const char *signature)
{
    bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const struct fe_acpi_sdt_header *root =
        (const struct fe_acpi_sdt_header *)fix_phys_ptr(
            (void *)(uptr)(use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address));
    if (!root || !checksum_ok(root, root->length)) {
        return NULL;
    }
    if (!sig_eq(root->signature, use_xsdt ? "XSDT" : "RSDT", 4)) {
        return NULL;
    }

    u32 entry_size = use_xsdt ? 8 : 4;
    u32 count = (root->length - sizeof(struct fe_acpi_sdt_header)) / entry_size;
    const u8 *entries = (const u8 *)root + sizeof(struct fe_acpi_sdt_header);

    for (u32 i = 0; i < count; i++) {
        u64 addr = use_xsdt ? *(const u64 *)(const void *)(entries + i * entry_size)
                            : (u64)(*(const u32 *)(const void *)(entries + i * entry_size));
        const struct fe_acpi_sdt_header *t =
            (const struct fe_acpi_sdt_header *)fix_phys_ptr((void *)(uptr)addr);
        if (!t || t->length < sizeof(struct fe_acpi_sdt_header)) {
            continue;
        }
        if (sig_eq(t->signature, signature, 4) && checksum_ok(t, t->length)) {
            return t;
        }
    }
    return NULL;
}

bool fe_acpi_init(void *rsdp_hint)
{
    memset(&g_info, 0, sizeof(g_info));

    const struct fe_acpi_rsdp *rsdp = (const struct fe_acpi_rsdp *)fix_phys_ptr(rsdp_hint);
    if (!rsdp || !sig_eq(rsdp->signature, "RSD PTR ", 8)) {
        /* 原值不是 RSDP，再试一次另一种地址解释 */
        u64 v = (u64)(uptr)rsdp_hint;
        u64 hhdm = fe_vmm_hhdm_offset();
        rsdp = (const struct fe_acpi_rsdp *)fix_phys_ptr(
            (void *)(uptr)((v >= hhdm) ? (v - hhdm) : v));
        if (!rsdp || !sig_eq(rsdp->signature, "RSD PTR ", 8)) {
            return false;
        }
    }
    if (!checksum_ok(rsdp, 20)) {
        return false;
    }

    const struct fe_acpi_madt *madt =
        (const struct fe_acpi_madt *)find_table(rsdp, "APIC");
    if (!madt) {
        return false;
    }

    g_info.lapic_address = madt->local_apic_address;
    g_info.pcat_compat = (madt->flags & 1u) != 0;

    /* 逐条解析变长条目 */
    const u8 *p = (const u8 *)madt + sizeof(struct fe_acpi_madt);
    const u8 *end = (const u8 *)madt + madt->header.length;
    while (p + 2 <= end) {
        u8 type = p[0];
        u8 length = p[1];
        if (length < 2 || p + length > end) {
            break;
        }
        switch (type) {
        case FE_MADT_LAPIC: {
            const struct fe_madt_lapic *e = (const struct fe_madt_lapic *)(const void *)p;
            if (length >= sizeof(*e) && g_info.cpu_count < FE_ACPI_MAX_CPUS) {
                /* flags bit0 = 该核可用 */
                if (e->flags & 1u) {
                    g_info.cpu_apic_ids[g_info.cpu_count++] = e->apic_id;
                }
            }
            break;
        }
        case FE_MADT_IOAPIC: {
            const struct fe_madt_ioapic *e = (const struct fe_madt_ioapic *)(const void *)p;
            if (length >= sizeof(*e) && g_info.ioapic_count < FE_ACPI_MAX_IOAPIC) {
                struct fe_acpi_ioapic *out = &g_info.ioapics[g_info.ioapic_count++];
                out->id = e->id;
                out->address = e->address;
                out->gsi_base = e->gsi_base;
            }
            break;
        }
        case FE_MADT_ISO: {
            const struct fe_madt_iso *e = (const struct fe_madt_iso *)(const void *)p;
            if (length >= sizeof(*e) && g_info.iso_count < FE_ACPI_MAX_ISO) {
                struct fe_acpi_iso *out = &g_info.isos[g_info.iso_count++];
                out->source = e->source;
                out->gsi = e->gsi;
                out->flags = e->flags;
            }
            break;
        }
        case FE_MADT_LAPIC_OVERRIDE: {
            const struct fe_madt_lapic_override *e =
                (const struct fe_madt_lapic_override *)(const void *)p;
            if (length >= sizeof(*e) && e->address != 0) {
                g_info.lapic_address = (u32)e->address;
            }
            break;
        }
        default:
            break;
        }
        p += length;
    }

    g_info.valid = g_info.lapic_address != 0;
    return g_info.valid;
}

void fe_acpi_irq_to_gsi(u8 irq, u32 *out_gsi, bool *out_level, bool *out_active_low)
{
    for (u32 i = 0; i < g_info.iso_count; i++) {
        if (g_info.isos[i].source == irq) {
            *out_gsi = g_info.isos[i].gsi;
            u16 f = g_info.isos[i].flags;
            *out_active_low = (f & 0x2) != 0;
            *out_level = (f & 0x8) != 0;
            return;
        }
    }
    /* 没有覆盖条目：ISA 中断默认低电平有效 + 电平触发，其余按边沿高有效 */
    *out_gsi = irq;
    *out_active_low = (irq < 16);
    *out_level = (irq < 16);
}

void fe_acpi_dump(void)
{
    if (!g_info.valid) {
        fe_kprintf("[ACPI] 未找到可用的 MADT\n");
        return;
    }
    fe_kprintf("[ACPI] LAPIC @ %#x, IOAPIC %u 个, CPU %u 个, 双 8259 模式: %s\n",
               g_info.lapic_address, g_info.ioapic_count, g_info.cpu_count,
               g_info.pcat_compat ? "是" : "否");
    for (u32 i = 0; i < g_info.ioapic_count; i++) {
        fe_kprintf("[ACPI]   IOAPIC[%u] id=%u @ %#x, GSI 基址 %u\n", i,
                   g_info.ioapics[i].id, g_info.ioapics[i].address,
                   g_info.ioapics[i].gsi_base);
    }
    for (u32 i = 0; i < g_info.iso_count; i++) {
        fe_kprintf("[ACPI]   中断源覆盖: IRQ%u -> GSI %u, %s, %s\n",
                   g_info.isos[i].source, g_info.isos[i].gsi,
                   (g_info.isos[i].flags & 0x8) ? "电平触发" : "边沿触发",
                   (g_info.isos[i].flags & 0x2) ? "低有效" : "高有效");
    }
}
