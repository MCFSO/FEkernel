/* SPDX-License-Identifier: 0BSD */
/* FEKernel 对 Limine 引导协议的实现。
 *
 * 定位说明：Limine 是**外部引导器**，其引导协议是公开接口规范（等价于一份 ABI 文档），
 * 不属于任何操作系统内核的代码。本文件是按该协议规范自行实现的结构定义，采用 FEKernel
 * 自己的命名与组织方式；其中的魔数（marker/id）必须与协议逐位一致，否则引导器无法识别。
 *
 * 协议要点：
 *   1. 内核在 .limine_requests_start 与 .limine_requests_end 之间放置若干「请求」结构；
 *   2. 请求的首 32 字节是 id[4] = {COMMON_MAGIC, 请求 ID}，随后是 revision 与 response 指针；
 *   3. 引导器扫描内核映像，找到这些请求并填写 response 指针；未实现的请求保持 NULL。
 *
 * 规范来源：Limine Boot Protocol（limine-protocol 仓库 PROTOCOL.md）。
 */
#ifndef FE_BOOT_LIMINE_H
#define FE_BOOT_LIMINE_H

#include <fe/types.h>
#include <fe/compiler.h>

/* ------------------------------------------------------------------ */
/* 请求区标记与版本                                                    */
/* ------------------------------------------------------------------ */

#define FE_LIMINE_REQUESTS_START_MARKER                                        \
    { 0xf6b8f4b39de7d1aeull, 0xfab91a6940fcb9cfull,                            \
      0x785c6ed015d3e316ull, 0x181e920a7852b9d9ull }

#define FE_LIMINE_REQUESTS_END_MARKER                                          \
    { 0xadc0e0531bb10d03ull, 0x9572709f31764c62ull }

/* 我们实现的协议基版本。引导器若不支持会把 [2] 改成它自己的最高版本。 */
#define FE_LIMINE_BASE_REVISION 3

#define FE_LIMINE_BASE_REVISION_ENTRY(n)                                       \
    { 0xf9562b2d5c95a6c8ull, 0x6a7b384944536bdcull, (n) }

/* 基版本被支持的条件：引导器把 [2] 写回 0 */
#define FE_LIMINE_BASE_REVISION_SUPPORTED(var) ((var)[2] == 0)

#define FE_LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88ull, 0x0a82e883a194f07bull

/* 请求编号（id[0..1] 为 COMMON_MAGIC，id[2..3] 为下述值） */
#define FE_LIMINE_ID_BOOTLOADER_INFO   0xf55038d8e2a1202full, 0x279426fcf5f59740ull
#define FE_LIMINE_ID_EXEC_CMDLINE      0x4b161536e598651eull, 0xb390ad4a2f1f303aull
#define FE_LIMINE_ID_FIRMWARE_TYPE     0x8c2f75d90bef28a8ull, 0x7045a4688eac00c3ull
#define FE_LIMINE_ID_STACK_SIZE        0x224ef0460a8e8926ull, 0xe1cb0fc25f46ea3dull
#define FE_LIMINE_ID_HHDM              0x48dcf1cb8ad2b852ull, 0x63984e959a98244bull
#define FE_LIMINE_ID_FRAMEBUFFER       0x9d5827dcd881dd75ull, 0xa3148604f6fab11bull
#define FE_LIMINE_ID_PAGING_MODE       0x95c1a0edab0944cbull, 0xa4e5cb3842f7488aull
#define FE_LIMINE_ID_MP                0x95a67b819a1b857eull, 0xa0b61b723b6a73e0ull
#define FE_LIMINE_ID_MEMMAP            0x67cf3d9d378a806full, 0xe304acdfc50c3c62ull
#define FE_LIMINE_ID_EXEC_FILE         0xad97e90e83f1ed67ull, 0x31eb5d1c5ff23b69ull
#define FE_LIMINE_ID_MODULE            0x3e7e279702be32afull, 0xca1c4f3bd1280ceeull
#define FE_LIMINE_ID_RSDP              0xc5e77b6b397e7b43ull, 0x27637845accdcf3cull
#define FE_LIMINE_ID_SMBIOS            0x9e9046f11e095391ull, 0xaa4a520fefbde5eeull
#define FE_LIMINE_ID_EFI_SYSTEM_TABLE  0x5ceba5163eaaf6d6ull, 0x0a6981610cf65fccull
#define FE_LIMINE_ID_EFI_MEMMAP        0x7df62a431d6872d5ull, 0xa4fcdfb3e57306c8ull
#define FE_LIMINE_ID_DATE_AT_BOOT      0x502746e184c088aaull, 0xfbc5ec83e6327893ull
#define FE_LIMINE_ID_EXEC_ADDRESS      0x71ba76863cc55f63ull, 0xb2644a48c516a487ull
#define FE_LIMINE_ID_TSC_FREQUENCY     0x10f2ee1d87d195e4ull, 0xf747a2b78f6ddb31ull
#define FE_LIMINE_ID_ENTROPY           0x65ea80255d5682c5ull, 0x9117240723f493ebull

/* 内存映射条目类型 */
enum fe_limine_memmap_type {
    FE_LIMMAP_USABLE                 = 0,
    FE_LIMMAP_RESERVED               = 1,
    FE_LIMMAP_ACPI_RECLAIMABLE       = 2,
    FE_LIMMAP_ACPI_NVS               = 3,
    FE_LIMMAP_BAD_MEMORY             = 4,
    FE_LIMMAP_BOOTLOADER_RECLAIMABLE = 5,
    FE_LIMMAP_EXEC_AND_MODULES       = 6,
    FE_LIMMAP_FRAMEBUFFER            = 7,
    FE_LIMMAP_RESERVED_MAPPED        = 8,
};

/* 固件类型 */
enum fe_limine_firmware_type {
    FE_LIMINE_FW_X86BIOS = 0,
    FE_LIMINE_FW_EFI32   = 1,
    FE_LIMINE_FW_EFI64   = 2,
    FE_LIMINE_FW_SBI     = 3,
};

/* 帧缓冲像素格式 */
#define FE_LIMINE_FB_RGB 1

#define FE_LIMINE_PAGING_X86_64_4LVL 0
#define FE_LIMINE_PAGING_X86_64_5LVL 1

/* ------------------------------------------------------------------ */
/* 通用子结构                                                          */
/* ------------------------------------------------------------------ */

struct fe_limine_uuid {
    u32 a;
    u16 b;
    u16 c;
    u8  d[8];
};

/* 由引导器加载的一个文件（内核自身、模块等） */
struct fe_limine_file {
    u64 revision;
    void *address;          /* 文件内容所在的虚拟地址（HHDM 区） */
    u64 size;
    char *path;
    char *string;           /* 引导配置里写的描述串 */
    u32 media_type;
    u32 unused;
    u8  tftp_ipv4[4];
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    struct fe_limine_uuid gpt_disk_uuid;
    struct fe_limine_uuid gpt_part_uuid;
    struct fe_limine_uuid part_uuid;
};

struct fe_limine_video_mode {
    u64 pitch;
    u64 width;
    u64 height;
    u16 bpp;
    u8  memory_model;
    u8  red_mask_size;
    u8  red_mask_shift;
    u8  green_mask_size;
    u8  green_mask_shift;
    u8  blue_mask_size;
    u8  blue_mask_shift;
};

struct fe_limine_framebuffer {
    void *address;          /* HHDM 中可直接写入的线性帧缓冲 */
    u64 width;
    u64 height;
    u64 pitch;              /* 每行字节数 */
    u16 bpp;
    u8  memory_model;
    u8  red_mask_size;
    u8  red_mask_shift;
    u8  green_mask_size;
    u8  green_mask_shift;
    u8  blue_mask_size;
    u8  blue_mask_shift;
    u8  unused[7];
    u64 edid_size;
    void *edid;
    u64 mode_count;         /* 响应 revision >= 1 */
    struct fe_limine_video_mode **modes;
};

struct fe_limine_memmap_entry {
    u64 base;
    u64 length;
    u64 type;
};

/* x86_64 的 CPU 信息（用于 SMP 启动 AP） */
struct fe_limine_mp_info {
    u32 processor_id;
    u32 lapic_id;
    u64 reserved;
    void (*goto_address)(struct fe_limine_mp_info *);
    u64 extra_argument;
};

/* ------------------------------------------------------------------ */
/* 请求与响应                                                          */
/* ------------------------------------------------------------------ */

struct fe_limine_bootloader_info_response { u64 revision; char *name; char *version; };
struct fe_limine_bootloader_info_request {
    u64 id[4]; u64 revision; struct fe_limine_bootloader_info_response *response;
};

struct fe_limine_exec_cmdline_response { u64 revision; char *cmdline; };
struct fe_limine_exec_cmdline_request {
    u64 id[4]; u64 revision; struct fe_limine_exec_cmdline_response *response;
};

struct fe_limine_firmware_type_response { u64 revision; u64 firmware_type; };
struct fe_limine_firmware_type_request {
    u64 id[4]; u64 revision; struct fe_limine_firmware_type_response *response;
};

struct fe_limine_hhdm_response { u64 revision; u64 offset; };
struct fe_limine_hhdm_request {
    u64 id[4]; u64 revision; struct fe_limine_hhdm_response *response;
};

struct fe_limine_framebuffer_response {
    u64 revision; u64 framebuffer_count; struct fe_limine_framebuffer **framebuffers;
};
struct fe_limine_framebuffer_request {
    u64 id[4]; u64 revision; struct fe_limine_framebuffer_response *response;
};

struct fe_limine_paging_mode_response { u64 revision; u64 mode; };
struct fe_limine_paging_mode_request {
    u64 id[4]; u64 revision; struct fe_limine_paging_mode_response *response;
    u64 mode; u64 max_mode; u64 min_mode;
};

struct fe_limine_memmap_response {
    u64 revision; u64 entry_count; struct fe_limine_memmap_entry **entries;
};
struct fe_limine_memmap_request {
    u64 id[4]; u64 revision; struct fe_limine_memmap_response *response;
};

struct fe_limine_exec_file_response { u64 revision; struct fe_limine_file *executable_file; };
struct fe_limine_exec_file_request {
    u64 id[4]; u64 revision; struct fe_limine_exec_file_response *response;
};

struct fe_limine_module_response {
    u64 revision; u64 module_count; struct fe_limine_file **modules;
};
struct fe_limine_module_request {
    u64 id[4]; u64 revision; struct fe_limine_module_response *response;
    u64 internal_module_count;              /* 请求 revision 1 才有意义，我们置 0 */
    void *internal_modules;
};

struct fe_limine_rsdp_response { u64 revision; void *address; };
struct fe_limine_rsdp_request {
    u64 id[4]; u64 revision; struct fe_limine_rsdp_response *response;
};

struct fe_limine_smbios_response { u64 revision; void *entry_32; void *entry_64; };
struct fe_limine_smbios_request {
    u64 id[4]; u64 revision; struct fe_limine_smbios_response *response;
};

struct fe_limine_efi_system_table_response { u64 revision; void *address; };
struct fe_limine_efi_system_table_request {
    u64 id[4]; u64 revision; struct fe_limine_efi_system_table_response *response;
};

struct fe_limine_efi_memmap_response {
    u64 revision; void *memmap; u64 memmap_size; u64 desc_size; u64 desc_version;
};
struct fe_limine_efi_memmap_request {
    u64 id[4]; u64 revision; struct fe_limine_efi_memmap_response *response;
};

struct fe_limine_date_at_boot_response { u64 revision; i64 timestamp; };
struct fe_limine_date_at_boot_request {
    u64 id[4]; u64 revision; struct fe_limine_date_at_boot_response *response;
};

struct fe_limine_exec_address_response {
    u64 revision; u64 physical_base; u64 virtual_base;
};
struct fe_limine_exec_address_request {
    u64 id[4]; u64 revision; struct fe_limine_exec_address_response *response;
};

struct fe_limine_mp_response {
    u64 revision; u32 flags; u32 bsp_lapic_id; u64 cpu_count;
    struct fe_limine_mp_info **cpus;
};
struct fe_limine_mp_request {
    u64 id[4]; u64 revision; struct fe_limine_mp_response *response; u64 flags;
};

struct fe_limine_tsc_frequency_response { u64 revision; u64 frequency; };
struct fe_limine_tsc_frequency_request {
    u64 id[4]; u64 revision; struct fe_limine_tsc_frequency_response *response;
};

struct fe_limine_entropy_response { u64 revision; u64 value_count; u64 *values; };
struct fe_limine_entropy_request {
    u64 id[4]; u64 revision; struct fe_limine_entropy_response *response;
    u64 value_count;
};

/* 请求段声明辅助宏：必须放在 .limine_requests 段中，且标记为 used 以免被优化掉。
 * 展开结果形如：static volatile struct X X = { ... };
 * 结构标签与变量同名在 C 中是合法的（二者处于不同的命名空间）。 */
#define FE_LIMINE_REQUEST(type)                                                \
    __attribute__((used, section(".limine_requests"))) static volatile struct type type

/* 引导器填好的全部响应指针，由 fe_limine_collect 一次性取出。
 * 未实现的请求对应字段为 NULL。 */
struct fe_limine_responses {
    struct fe_limine_bootloader_info_response *bootloader_info;
    struct fe_limine_exec_cmdline_response    *cmdline;
    struct fe_limine_firmware_type_response   *firmware_type;
    struct fe_limine_hhdm_response            *hhdm;
    struct fe_limine_framebuffer_response     *framebuffer;
    struct fe_limine_paging_mode_response     *paging_mode;
    struct fe_limine_memmap_response          *memmap;
    struct fe_limine_exec_file_response       *exec_file;
    struct fe_limine_module_response          *module;
    struct fe_limine_rsdp_response            *rsdp;
    struct fe_limine_smbios_response          *smbios;
    struct fe_limine_efi_system_table_response *efi_system_table;
    struct fe_limine_efi_memmap_response      *efi_memmap;
    struct fe_limine_date_at_boot_response    *date_at_boot;
    struct fe_limine_exec_address_response    *exec_address;
    struct fe_limine_mp_response              *mp;
    struct fe_limine_tsc_frequency_response   *tsc_frequency;
    struct fe_limine_entropy_response         *entropy;
    bool base_revision_supported;
    u64  loaded_base_revision;
};

void fe_limine_collect(struct fe_limine_responses *out);

#define FE_LIMINE_REQUESTS_START                                               \
    __attribute__((used, section(".limine_requests_start")))                   \
    static volatile u64 fe_limine_requests_start_marker[4] =                   \
        FE_LIMINE_REQUESTS_START_MARKER

#define FE_LIMINE_REQUESTS_END                                                 \
    __attribute__((used, section(".limine_requests_end")))                     \
    static volatile u64 fe_limine_requests_end_marker[2] =                     \
        FE_LIMINE_REQUESTS_END_MARKER

#endif /* FE_BOOT_LIMINE_H */
