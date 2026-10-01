/* SPDX-License-Identifier: 0BSD */
#include <fe/boot/bootinfo.h>
#include <fe/boot/limine.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/panic.h>

static struct fe_boot_info g_boot_info;

const struct fe_boot_info *fe_boot_info(void)
{
    return &g_boot_info;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

bool fe_boot_info_init(void)
{
    struct fe_limine_responses res;
    memset(&g_boot_info, 0, sizeof(g_boot_info));

    fe_limine_collect(&res);
    g_boot_info.base_revision_ok = res.base_revision_supported;
    g_boot_info.loaded_base_revision = res.loaded_base_revision;

    if (!res.base_revision_supported) {
        /* 引导器把 [2] 改写成了它自己的最高版本号，说明不理解我们要求的版本 */
        return false;
    }

    if (res.bootloader_info) {
        copy_string(g_boot_info.bootloader_name, sizeof(g_boot_info.bootloader_name),
                    res.bootloader_info->name);
        copy_string(g_boot_info.bootloader_version, sizeof(g_boot_info.bootloader_version),
                    res.bootloader_info->version);
    }
    if (res.cmdline) {
        copy_string(g_boot_info.cmdline, sizeof(g_boot_info.cmdline), res.cmdline->cmdline);
    }
    if (res.firmware_type) {
        g_boot_info.firmware_type = (u32)res.firmware_type->firmware_type;
    }
    if (res.hhdm) {
        g_boot_info.hhdm_offset = res.hhdm->offset;
    }
    if (res.exec_address) {
        g_boot_info.kernel_phys_base = res.exec_address->physical_base;
        g_boot_info.kernel_virt_base = res.exec_address->virtual_base;
    }
    if (res.exec_file && res.exec_file->executable_file) {
        g_boot_info.kernel_file_size = res.exec_file->executable_file->size;
        g_boot_info.kernel_elf = res.exec_file->executable_file->address;
    }
    if (res.tsc_frequency) {
        g_boot_info.tsc_frequency = res.tsc_frequency->frequency;
    }
    if (res.date_at_boot) {
        g_boot_info.boot_timestamp = res.date_at_boot->timestamp;
    }
    if (res.rsdp) {
        g_boot_info.rsdp = res.rsdp->address;
    }
    if (res.smbios) {
        g_boot_info.smbios_entry_64 = res.smbios->entry_64;
    }
    if (res.efi_system_table) {
        g_boot_info.efi_system_table = res.efi_system_table->address;
    }
    if (res.mp) {
        g_boot_info.bsp_lapic_id = res.mp->bsp_lapic_id;
        g_boot_info.cpu_count = res.mp->cpu_count;
    }

    /* 帧缓冲：只取第一个（多显示器时取主显示器） */
    if (res.framebuffer && res.framebuffer->framebuffer_count > 0 &&
        res.framebuffer->framebuffers && res.framebuffer->framebuffers[0]) {
        const struct fe_limine_framebuffer *fb = res.framebuffer->framebuffers[0];
        struct fe_framebuffer_info *out = &g_boot_info.fb;
        out->address = fb->address;
        out->width = fb->width;
        out->height = fb->height;
        out->pitch = fb->pitch;
        out->bpp = fb->bpp;
        out->memory_model = fb->memory_model;
        out->red_mask_size = fb->red_mask_size;
        out->red_mask_shift = fb->red_mask_shift;
        out->green_mask_size = fb->green_mask_size;
        out->green_mask_shift = fb->green_mask_shift;
        out->blue_mask_size = fb->blue_mask_size;
        out->blue_mask_shift = fb->blue_mask_shift;
        out->size_bytes = fb->pitch * fb->height;
        g_boot_info.has_framebuffer = true;
    }

    /* 物理内存映射 */
    if (res.memmap) {
        u64 n = res.memmap->entry_count;
        if (n > FE_BOOTINFO_MAX_MEMMAP) {
            n = FE_BOOTINFO_MAX_MEMMAP;
        }
        for (u64 i = 0; i < n; i++) {
            const struct fe_limine_memmap_entry *e = res.memmap->entries[i];
            if (!e) {
                continue;
            }
            g_boot_info.memmap[g_boot_info.memmap_count].base = e->base;
            g_boot_info.memmap[g_boot_info.memmap_count].length = e->length;
            g_boot_info.memmap[g_boot_info.memmap_count].type = (u32)e->type;
            g_boot_info.memmap_count++;

            u64 end = e->base + e->length;
            if (end > g_boot_info.highest_address) {
                g_boot_info.highest_address = end;
            }
            if (e->type == FE_LIMMAP_USABLE) {
                g_boot_info.total_usable_bytes += e->length;
            }
        }
    }

    /* 模块：用户态服务镜像 */
    if (res.module && res.module->modules) {
        u64 n = res.module->module_count;
        if (n > FE_BOOTINFO_MAX_MODULES) {
            n = FE_BOOTINFO_MAX_MODULES;
        }
        for (u64 i = 0; i < n; i++) {
            const struct fe_limine_file *f = res.module->modules[i];
            if (!f) {
                continue;
            }
            struct fe_module_info *m = &g_boot_info.modules[g_boot_info.module_count];
            m->path = f->path;
            m->cmdline = f->string;
            m->address = f->address;
            m->size = f->size;
            g_boot_info.module_count++;
        }
    }

    return true;
}

static const char *firmware_type_name(u32 t)
{
    switch (t) {
    case FE_LIMINE_FW_X86BIOS: return "x86 BIOS (legacy)";
    case FE_LIMINE_FW_EFI32:   return "UEFI 32 位";
    case FE_LIMINE_FW_EFI64:   return "UEFI 64 位";
    case FE_LIMINE_FW_SBI:     return "RISC-V SBI";
    default:                   return "未知";
    }
}

static const char *memmap_type_name(u32 t)
{
    switch (t) {
    case FE_LIMMAP_USABLE:                 return "可用";
    case FE_LIMMAP_RESERVED:               return "保留";
    case FE_LIMMAP_ACPI_RECLAIMABLE:       return "ACPI 可回收";
    case FE_LIMMAP_ACPI_NVS:               return "ACPI NVS";
    case FE_LIMMAP_BAD_MEMORY:             return "坏内存";
    case FE_LIMMAP_BOOTLOADER_RECLAIMABLE: return "引导器可回收";
    case FE_LIMMAP_EXEC_AND_MODULES:       return "内核与模块";
    case FE_LIMMAP_FRAMEBUFFER:            return "帧缓冲";
    case FE_LIMMAP_RESERVED_MAPPED:        return "保留(已映射)";
    default:                               return "?";
    }
}

void fe_boot_info_dump(void)
{
    const struct fe_boot_info *bi = &g_boot_info;

    fe_kprintf("[引导] %s %s, 固件: %s\n",
               bi->bootloader_name[0] ? bi->bootloader_name : "(未知)",
               bi->bootloader_version, firmware_type_name(bi->firmware_type));
    fe_kprintf("[引导] 协议基版本 %llu (期望 %d) — %s\n",
               (unsigned long long)bi->loaded_base_revision, FE_BOOT_PROTOCOL_BASE_REVISION,
               bi->base_revision_ok ? "兼容" : "不兼容");
    if (bi->cmdline[0]) {
        fe_kprintf("[引导] 命令行: %s\n", bi->cmdline);
    }

    fe_kprintf("[内存] HHDM 偏移   = %#llx\n", (unsigned long long)bi->hhdm_offset);
    fe_kprintf("[内存] 内核物理基址 = %#llx, 虚拟基址 = %#llx, 映像 %llu KiB\n",
               (unsigned long long)bi->kernel_phys_base,
               (unsigned long long)bi->kernel_virt_base,
               (unsigned long long)(bi->kernel_file_size / 1024));
    fe_kprintf("[内存] 可用内存 %llu MiB / 最高地址 %#llx / 映射条目 %llu\n",
               (unsigned long long)(bi->total_usable_bytes / FE_MIB(1)),
               (unsigned long long)bi->highest_address,
               (unsigned long long)bi->memmap_count);

    fe_kprintf("[内存] 物理内存映射:\n");
    for (u64 i = 0; i < bi->memmap_count; i++) {
        const struct fe_memmap_region *r = &bi->memmap[i];
        fe_kprintf("        %016llx - %016llx  %8llu KiB  %s\n",
                   (unsigned long long)r->base,
                   (unsigned long long)(r->base + r->length),
                   (unsigned long long)(r->length / 1024),
                   memmap_type_name(r->type));
    }

    if (bi->has_framebuffer) {
        const struct fe_framebuffer_info *fb = &bi->fb;
        fe_kprintf("[显示] 帧缓冲 %llux%llu %u bpp, 行距 %llu, 地址 %p, 共 %llu MiB\n",
                   (unsigned long long)fb->width, (unsigned long long)fb->height,
                   fb->bpp, (unsigned long long)fb->pitch, fb->address,
                   (unsigned long long)(fb->size_bytes / FE_MIB(1)));
        fe_kprintf("[显示] 像素格式: %s R%u@%u G%u@%u B%u@%u\n",
                   fb->memory_model == FE_LIMINE_FB_RGB ? "RGB" : "其它",
                   fb->red_mask_size, fb->red_mask_shift,
                   fb->green_mask_size, fb->green_mask_shift,
                   fb->blue_mask_size, fb->blue_mask_shift);
    } else {
        fe_kprintf("[显示] 无帧缓冲\n");
    }

    fe_kprintf("[CPU ] 逻辑 CPU %llu 个, BSP LAPIC ID %u, TSC %llu MHz\n",
               (unsigned long long)bi->cpu_count, bi->bsp_lapic_id,
               (unsigned long long)(bi->tsc_frequency / 1000000));
    fe_kprintf("[固件] RSDP %p, SMBIOS %p, EFI 系统表 %p\n",
               bi->rsdp, bi->smbios_entry_64, bi->efi_system_table);
    fe_kprintf("[时间] 启动时刻时间戳 %lld\n", (long long)bi->boot_timestamp);

    if (bi->module_count) {
        fe_kprintf("[模块] 共 %llu 个:\n", (unsigned long long)bi->module_count);
        for (u64 i = 0; i < bi->module_count; i++) {
            const struct fe_module_info *m = &bi->modules[i];
            fe_kprintf("        %s (%llu KiB) @ %p\n",
                       m->path ? m->path : "(无名)",
                       (unsigned long long)(m->size / 1024), m->address);
        }
    } else {
        fe_kprintf("[模块] 无（用户态服务镜像尚未随内核加载）\n");
    }
}
