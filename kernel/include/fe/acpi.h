/* SPDX-License-Identifier: 0BSD */
/* ACPI 表解析（M2 只需要 MADT：多处理器与中断控制器描述表）。
 *
 * 用途：找到 LAPIC 与 IOAPIC 的 MMIO 地址、CPU 列表、以及 ISA 中断源覆盖（ISO）。
 * 中断线在真实硬件上并不严格对应 IRQ 号，必须以 MADT 的 ISO 为准——这是 M2 之后
 * 所有外设中断能正常工作的前提。
 */
#ifndef FE_ACPI_H
#define FE_ACPI_H

#include <fe/types.h>
#include <fe/compiler.h>

#define FE_ACPI_MAX_CPUS 64
#define FE_ACPI_MAX_ISO  16
#define FE_ACPI_MAX_IOAPIC 4

struct fe_acpi_rsdp {
    char signature[8];      /* "RSD PTR " */
    u8   checksum;
    char oem_id[6];
    u8   revision;
    u32  rsdt_address;
    u32  length;
    u64  xsdt_address;
    u8   extended_checksum;
    u8   reserved[3];
} FE_PACKED;

struct fe_acpi_sdt_header {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} FE_PACKED;

struct fe_acpi_madt {
    struct fe_acpi_sdt_header header;
    u32 local_apic_address;
    u32 flags;
} FE_PACKED;

/* MADT 条目类型 */
#define FE_MADT_LAPIC          0
#define FE_MADT_IOAPIC         1
#define FE_MADT_ISO            2
#define FE_MADT_NMI_SOURCE     3
#define FE_MADT_LAPIC_OVERRIDE 5
#define FE_MADT_LAPIC_NMI      4
#define FE_MADT_X2APIC         9

struct fe_madt_lapic {
    u8 type; u8 length; u8 acpi_processor_id; u8 apic_id; u32 flags;
} FE_PACKED;

struct fe_madt_ioapic {
    u8 type; u8 length; u8 id; u8 reserved; u32 address; u32 gsi_base;
} FE_PACKED;

struct fe_madt_iso {
    u8 type; u8 length; u8 bus; u8 source; u32 gsi; u16 flags;
} FE_PACKED;

struct fe_madt_lapic_override {
    u8 type; u8 length; u16 reserved; u64 address;
} FE_PACKED;

struct fe_acpi_iso {
    u8  source;     /* ISA IRQ 号 */
    u32 gsi;        /* 对应全局系统中断号 */
    u16 flags;      /* bit1 = 低电平有效, bit3 = 电平触发 */
};

struct fe_acpi_ioapic {
    u8  id;
    u32 address;
    u32 gsi_base;
};

struct fe_acpi_info {
    bool valid;
    bool pcat_compat;           /* MADT flags bit0：系统带双 8259 */
    u32  lapic_address;         /* 已被 LAPIC Override 条目修正 */
    u32  ioapic_count;
    struct fe_acpi_ioapic ioapics[FE_ACPI_MAX_IOAPIC];
    u32  cpu_count;
    u32  cpu_apic_ids[FE_ACPI_MAX_CPUS];
    u32  bsp_apic_id;
    u32  iso_count;
    struct fe_acpi_iso isos[FE_ACPI_MAX_ISO];
};

/* 解析 RSDP → MADT，填充 fe_acpi_info。成功返回 true。 */
bool fe_acpi_init(void *rsdp_hint);

const struct fe_acpi_info *fe_acpi_info(void);

/* 查询某个 ISA IRQ 对应的 GSI 与触发/极性标志；没有覆盖条目时按恒等映射处理 */
void fe_acpi_irq_to_gsi(u8 irq, u32 *out_gsi, bool *out_level, bool *out_active_low);

void fe_acpi_dump(void);

#endif /* FE_ACPI_H */
