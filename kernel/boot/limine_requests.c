/* SPDX-License-Identifier: 0BSD */
/* 各个 Limine 请求的实例定义。
 *
 * 这些结构必须位于 .limine_requests 段（由 linker.ld 单独放在可写段中），
 * 引导器会在跳转到内核之前填入 response 指针。
 */
#include <fe/boot/limine.h>

/* 请求区边界标记 */
FE_LIMINE_REQUESTS_START;

/* 协议基版本：引导器支持则把 [2] 写回 0 */
__attribute__((used, section(".limine_requests")))
static volatile u64 fe_limine_base_revision[3] = FE_LIMINE_BASE_REVISION_ENTRY(FE_LIMINE_BASE_REVISION);

/* 引导器自身信息 */
FE_LIMINE_REQUEST(fe_limine_bootloader_info_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_BOOTLOADER_INFO },
    .revision = 0,
    .response = NULL,
};

/* 内核命令行 */
FE_LIMINE_REQUEST(fe_limine_exec_cmdline_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_EXEC_CMDLINE },
    .revision = 0,
    .response = NULL,
};

/* 固件类型（BIOS / UEFI32 / UEFI64） */
FE_LIMINE_REQUEST(fe_limine_firmware_type_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_FIRMWARE_TYPE },
    .revision = 0,
    .response = NULL,
};

/* 直接物理内存映射（HHDM）偏移 */
FE_LIMINE_REQUEST(fe_limine_hhdm_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_HHDM },
    .revision = 0,
    .response = NULL,
};

/* 帧缓冲：内核启动早期就能拿到，用于早期图形控制台 */
FE_LIMINE_REQUEST(fe_limine_framebuffer_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_FRAMEBUFFER },
    .revision = 0,
    .response = NULL,
};

/* 分页模式：我们固定用 4 级页表 */
FE_LIMINE_REQUEST(fe_limine_paging_mode_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_PAGING_MODE },
    .revision = 0,
    .response = NULL,
    .mode = FE_LIMINE_PAGING_X86_64_4LVL,
    .max_mode = FE_LIMINE_PAGING_X86_64_5LVL,
    .min_mode = FE_LIMINE_PAGING_X86_64_4LVL,
};

/* 物理内存映射表 */
FE_LIMINE_REQUEST(fe_limine_memmap_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_MEMMAP },
    .revision = 0,
    .response = NULL,
};

/* 内核自身的文件信息（用于定位映像范围） */
FE_LIMINE_REQUEST(fe_limine_exec_file_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_EXEC_FILE },
    .revision = 0,
    .response = NULL,
};

/* 模块：用户态服务以模块形式随内核一起被加载 */
FE_LIMINE_REQUEST(fe_limine_module_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_MODULE },
    .revision = 0,
    .response = NULL,
    .internal_module_count = 0,
    .internal_modules = NULL,
};

/* ACPI RSDP：电源管理、中断控制器与定时器发现都要用 */
FE_LIMINE_REQUEST(fe_limine_rsdp_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_RSDP },
    .revision = 0,
    .response = NULL,
};

/* SMBIOS：机型信息，用户在图形控制台与日志里能看到 */
FE_LIMINE_REQUEST(fe_limine_smbios_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_SMBIOS },
    .revision = 0,
    .response = NULL,
};

/* EFI 系统表：UEFI 下需要用它做运行时服务（重启、关机、变量） */
FE_LIMINE_REQUEST(fe_limine_efi_system_table_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_EFI_SYSTEM_TABLE },
    .revision = 0,
    .response = NULL,
};

/* EFI 内存映射：固件保留区的权威来源 */
FE_LIMINE_REQUEST(fe_limine_efi_memmap_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_EFI_MEMMAP },
    .revision = 0,
    .response = NULL,
};

/* 启动时刻的 UTC 时间戳 */
FE_LIMINE_REQUEST(fe_limine_date_at_boot_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_DATE_AT_BOOT },
    .revision = 0,
    .response = NULL,
};

/* 内核被加载到的物理/虚拟基址：建立自有页表时需要 */
FE_LIMINE_REQUEST(fe_limine_exec_address_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_EXEC_ADDRESS },
    .revision = 0,
    .response = NULL,
};

/* 多处理器信息：SMP 阶段启动 AP 用；现在只要 BSP 的 LAPIC ID */
FE_LIMINE_REQUEST(fe_limine_mp_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_MP },
    .revision = 0,
    .response = NULL,
    .flags = 0,
};

/* TSC 频率：由固件/引导器校准，精度远高于自己猜 */
FE_LIMINE_REQUEST(fe_limine_tsc_frequency_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_TSC_FREQUENCY },
    .revision = 0,
    .response = NULL,
};

/* 引导器提供的熵：早期随机数种子 */
FE_LIMINE_REQUEST(fe_limine_entropy_request) = {
    .id = { FE_LIMINE_COMMON_MAGIC, FE_LIMINE_ID_ENTROPY },
    .revision = 0,
    .response = NULL,
    .value_count = 0,
};

FE_LIMINE_REQUESTS_END;

/* 把引导器填好的响应指针一次性交给上层解析。
 * 这里读的是 volatile 请求结构的 response 字段：引导器可能在我们执行到这里之前
 * 刚刚写入，因此必须原样读取，不能假设编译器缓存过。 */
void fe_limine_collect(struct fe_limine_responses *out)
{
    out->bootloader_info   = fe_limine_bootloader_info_request.response;
    out->cmdline           = fe_limine_exec_cmdline_request.response;
    out->firmware_type     = fe_limine_firmware_type_request.response;
    out->hhdm              = fe_limine_hhdm_request.response;
    out->framebuffer       = fe_limine_framebuffer_request.response;
    out->paging_mode       = fe_limine_paging_mode_request.response;
    out->memmap            = fe_limine_memmap_request.response;
    out->exec_file         = fe_limine_exec_file_request.response;
    out->module            = fe_limine_module_request.response;
    out->rsdp              = fe_limine_rsdp_request.response;
    out->smbios            = fe_limine_smbios_request.response;
    out->efi_system_table  = fe_limine_efi_system_table_request.response;
    out->efi_memmap        = fe_limine_efi_memmap_request.response;
    out->date_at_boot      = fe_limine_date_at_boot_request.response;
    out->exec_address      = fe_limine_exec_address_request.response;
    out->mp                = fe_limine_mp_request.response;
    out->tsc_frequency     = fe_limine_tsc_frequency_request.response;
    out->entropy           = fe_limine_entropy_request.response;

    out->base_revision_supported = FE_LIMINE_BASE_REVISION_SUPPORTED(fe_limine_base_revision);
    out->loaded_base_revision = fe_limine_base_revision[1];
}
