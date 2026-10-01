/* SPDX-License-Identifier: 0BSD */
/* 引导信息归一化层。
 *
 * 目的：让内核其余部分（以及后续的 init 进程）完全不依赖 Limine 的具体数据结构。
 * limine 的响应在这里被解析成 FEKernel 自己的 fe_boot_info 格式，之后内核内部只认这一份。
 */
#ifndef FE_BOOT_BOOTINFO_H
#define FE_BOOT_BOOTINFO_H

#include <fe/types.h>

#define FE_BOOTINFO_MAX_MEMMAP  192
#define FE_BOOTINFO_MAX_MODULES 32

/* 内核要求的最低引导协议基版本 */
#define FE_BOOT_PROTOCOL_BASE_REVISION 3

/* 帧缓冲像素模型（取值与引导协议一致） */
#define FE_FB_MODEL_RGB 1

struct fe_framebuffer_info {
    void *address;      /* HHDM 中的线性帧缓冲地址 */
    u64   width;
    u64   height;
    u64   pitch;
    u16   bpp;
    u8    memory_model;
    u8    red_mask_size, red_mask_shift;
    u8    green_mask_size, green_mask_shift;
    u8    blue_mask_size, blue_mask_shift;
    u64   size_bytes;
};

/* 交给用户态控制台服务的帧缓冲信息（与 user/include/fe_user.h 的
 * struct fe_fb_info **逐字节一致**，经 FE_SYS_FB_INFO 传出）。
 *
 * ★ 这里是物理地址 ★ 用户态要用 fe_mmio_map 映射它，而 MMIO 必须关缓存；
 * 内核那条 HHDM 线性地址是内核自用的，给出去就是错的。
 *
 * 像素格式的三个 mask/shift 也一起给：值 0xF800/0x07E0/0x001F 这类
 * 16 位格式与 32 位 RGB 的差别只有靠它们才能表达，
 * 让用户态"自己按 bpp 猜"在换机器时必然出错。 */
struct fe_fb_info {
    u64 phys;
    u64 size_bytes;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u8  memory_model;
    u8  red_size,   red_shift;
    u8  green_size, green_shift;
    u8  blue_size,  blue_shift;
    u8  _pad[5];
};

struct fe_memmap_region {
    phys_addr_t base;
    u64         length;
    u32         type;       /* enum fe_limine_memmap_type */
};

struct fe_module_info {
    const char *path;
    const char *cmdline;
    void       *address;    /* HHDM 中的映像地址 */
    u64         size;
};

struct fe_boot_info {
    /* 引导器与固件 */
    char bootloader_name[64];
    char bootloader_version[64];
    char cmdline[256];
    u32  firmware_type;
    bool base_revision_ok;
    u64  loaded_base_revision;

    /* 内存布局 */
    u64  hhdm_offset;
    u64  kernel_phys_base;
    u64  kernel_virt_base;
    u64  kernel_file_size;
    void *kernel_elf;       /* 内核自身的 ELF 映像（HHDM 地址），建立自有页表时按段解析 */
    u64  total_usable_bytes;
    u64  highest_address;
    u64  memmap_count;
    struct fe_memmap_region memmap[FE_BOOTINFO_MAX_MEMMAP];

    /* 显示 */
    bool has_framebuffer;
    struct fe_framebuffer_info fb;

    /* CPU 与时钟 */
    u32  bsp_lapic_id;
    u64  cpu_count;
    u64  tsc_frequency;     /* 0 表示引导器未提供 */

    /* 固件表 */
    void *rsdp;
    void *smbios_entry_64;
    void *efi_system_table;
    i64   boot_timestamp;

    /* 随内核一起加载的用户态服务镜像 */
    u64  module_count;
    struct fe_module_info modules[FE_BOOTINFO_MAX_MODULES];
};

/* 解析引导器响应并填充全局引导信息。必须在任何内存管理初始化之前调用。
 * 返回 false 表示协议基版本不受支持，内核无法安全继续。 */
bool fe_boot_info_init(void);

/* 只读访问已解析的引导信息 */
const struct fe_boot_info *fe_boot_info(void);

/* 打印引导信息摘要（调试用） */
void fe_boot_info_dump(void);

#endif /* FE_BOOT_BOOTINFO_H */
