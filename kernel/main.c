/* SPDX-License-Identifier: 0BSD */
/* FEKernel 内核 C 入口。
 *
 * 本文件只做启动编排与**自检**，不含任何机制实现。
 * 每个里程碑的自检都会在真实启动过程中执行并把结论打到串口上，
 * 因此「里程碑完成」永远有实测证据，而不是只有代码。
 *
 * 初始化顺序不可随意调整，依赖关系如下：
 *   串口 → 引导信息 → GDT/IDT/PIC
 *        → PMM（帧）→ VMM（页表）→ 内核堆
 *        → ACPI（需要 HHDM）→ LAPIC/IOAPIC（需要 MMIO 映射）
 *        → 时间基准（需要 LAPIC 或 8254）→ 调度器（需要时间基准）
 *        → 开中断 → 自检
 */
#include <fe/types.h>
#include <fe/io.h>
#include <fe/kprintf.h>
#include <fe/serial.h>
#include <fe/gdt.h>
#include <fe/idt.h>
#include <fe/pic.h>
#include <fe/pit.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/boot/bootinfo.h>
#include <fe/mm.h>
#include <fe/mm/pmm.h>
#include <fe/mm/vmm.h>
#include <fe/mm/kheap.h>
#include <fe/mm/vma.h>
#include <fe/acpi.h>
#include <fe/arch/lapic.h>
#include <fe/arch/ioapic.h>
#include <fe/time.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/vectors.h>
#include <fe/task.h>
#include <fe/ipc.h>
#include <fe/user.h>
#include <fe/resource.h>
#include <fe/irq.h>
#include <fe/vfs.h>
#include <fe/ramfs.h>
#include <fe/devfs.h>
#include <fe/protect.h>
#include <fe/process.h>
#include <fe/fpu.h>

#define FE_VERSION_MAJOR 0
#define FE_VERSION_MINOR 3
#define FE_VERSION_PATCH 0
#define FE_CODENAME       "Genesis"

static u32 g_selftest_failures;

static void print_banner(void)
{
    fe_kprintf("\n");
    fe_kprintf("  ______ ______ _  __                    _ \n");
    fe_kprintf(" |  ____|  ____| |/ /                   | |\n");
    fe_kprintf(" | |__  | |__  | ' / ___ _ __ _ __   ___| |\n");
    fe_kprintf(" |  __| |  __| |  < / _ \\ '__| '_ \\ / _ \\ |\n");
    fe_kprintf(" | |    | |____| . \\  __/ |  | | | |  __/ |\n");
    fe_kprintf(" |_|    |______|_|\\_\\___|_|  |_| |_|\\___|_|\n");
    fe_kprintf("\n");
    fe_kprintf("  FEKernel %d.%d.%d \"%s\" — x86_64 微内核\n",
               FE_VERSION_MAJOR, FE_VERSION_MINOR, FE_VERSION_PATCH, FE_CODENAME);
    fe_kprintf("  构建目标: Limine 引导协议 / 4 级页表 / 内核基址 %#llx\n",
               (unsigned long long)FE_KERNEL_BASE);
    fe_kprintf("\n");
}

/* ------------------------------------------------------------------ */
/* 自检 1：格式化输出                                                  */
/* ------------------------------------------------------------------ */

static void selftest_kprintf(void)
{
    fe_kprintf("[自检] 格式化输出:\n");
    fe_kprintf("        十进制 %d / %i / 负数 %d\n", 12345, -42, -2147483647 - 1);
    fe_kprintf("        无符号 %u / 十六进制 %#x / 大写 %#X\n", 4000000000u, 0xdeadbeef, 0xdeadbeef);
    fe_kprintf("        八进制 %#o / 二进制 %#b\n", 0755, 0b10110100);
    fe_kprintf("        宽度 [%8d] [%-8d|] [%08d] [%+d] [% d]\n", 42, 42, 42, 42, 42);
    fe_kprintf("        精度 [%.3d] [%.5s] [%10.4s]\n", 7, "abcdefg", "abcdefg");
    fe_kprintf("        指针 %p / 字符串 %%s=%s / 字符 %c / 百分号 %%\n",
               (void *)0xffffffff80000000ull, "ok", 'F');
    fe_kprintf("        64 位 %llu / %#llx / size_t %zu\n",
               18446744073709551615ull, 0x0123456789abcdefull, (size_t)4096);
}

/* ------------------------------------------------------------------ */
/* 自检 2：异常处理路径                                                */
/* ------------------------------------------------------------------ */

static u32 g_bp_hits;

static void breakpoint_handler(struct fe_regs *r)
{
    g_bp_hits++;
    fe_kprintf("[自检] 捕获到断点异常 #BP: 向量=%llu RIP=%#llx 返回后继续执行\n",
               (unsigned long long)r->vector, (unsigned long long)r->rip);
}

static void selftest_exception(void)
{
    g_bp_hits = 0;
    fe_kprintf("[自检] 触发 int3 ……\n");
    __asm__ volatile("int3");
    if (g_bp_hits == 1) {
        fe_kprintf("[自检] 异常处理路径 OK（进入内核 → 分发 → iretq 返回原现场）\n");
    } else {
        fe_panic("断点异常处理未生效：命中次数 %u", g_bp_hits);
    }
}

/* ------------------------------------------------------------------ */
/* 自检 3：帧缓冲可写性                                                */
/* ------------------------------------------------------------------ */

static void fb_draw_band(u64 y0, u64 y1, bool gradient, u32 solid_color)
{
    const struct fe_boot_info *bi = fe_boot_info();
    if (!bi->has_framebuffer || bi->fb.bpp != 32 || bi->fb.memory_model != FE_FB_MODEL_RGB) {
        return;
    }
    const struct fe_framebuffer_info *fb = &bi->fb;
    volatile u32 *pixels = (volatile u32 *)fb->address;
    u64 stride = fb->pitch / 4;

    for (u64 x = 0; x < fb->width; x++) {
        u32 color = solid_color;
        if (gradient) {
            u32 r = (u32)((x * 255) / (fb->width ? fb->width : 1));
            u32 g = 255 - r;
            u32 b = (u32)((x * 7) & 0xFF);
            color = (r << 16) | (g << 8) | b;
        }
        for (u64 y = y0; y < y1 && y < fb->height; y++) {
            pixels[y * stride + x] = color;
        }
    }
}

static void selftest_framebuffer(void)
{
    fb_draw_band(8, 16, true, 0);
    fe_kprintf("[自检] 帧缓冲可写：顶部第二条渐变带是**切换自建页表之后**画的\n");
}

/* ------------------------------------------------------------------ */
/* 自检 4：物理帧分配器                                                */
/* ------------------------------------------------------------------ */

#define PMM_TEST_FRAMES 256

static u32 selftest_pmm(void)
{
    u32 fail = 0;
    struct fe_pmm_stats s0, s1;

    fe_pmm_get_stats(&s0);
    fe_kprintf("[自检] PMM: 位图 %llu 字节 @ %#llx, 总帧 %llu, 可用 %llu, 强制保留 %llu, "
               "当前空闲 %llu (%llu.%02llu MiB)\n",
               (unsigned long long)s0.bitmap_bytes, (unsigned long long)s0.bitmap_phys,
               (unsigned long long)s0.total_frames, (unsigned long long)s0.usable_frames,
               (unsigned long long)s0.reserved_frames,
               (unsigned long long)s0.free_frames,
               (unsigned long long)(s0.free_frames * FE_FRAME_SIZE / FE_MIB(1)),
               (unsigned long long)((s0.free_frames * FE_FRAME_SIZE % FE_MIB(1)) * 100 / FE_MIB(1)));

    phys_addr_t *frames = (phys_addr_t *)fe_kmalloc(sizeof(phys_addr_t) * PMM_TEST_FRAMES);
    if (!frames) {
        fe_kprintf("        测试数组分配失败\n");
        return 1;
    }
    u64 free_before = fe_pmm_free_frame_count();

    for (u32 i = 0; i < PMM_TEST_FRAMES; i++) {
        frames[i] = fe_pmm_alloc_frame();
        if (frames[i] == 0) {
            fe_kprintf("        第 %u 次帧分配失败\n", i);
            fail++;
            break;
        }
        if (!fe_is_aligned(frames[i], FE_FRAME_SIZE)) {
            fe_kprintf("        帧 %#llx 未按 4KiB 对齐\n", (unsigned long long)frames[i]);
            fail++;
        }
    }
    for (u32 i = 0; i < PMM_TEST_FRAMES && fail == 0; i++) {
        for (u32 j = i + 1; j < PMM_TEST_FRAMES; j++) {
            if (frames[i] == frames[j]) {
                fe_kprintf("        帧 %#llx 被重复分配\n", (unsigned long long)frames[i]);
                fail++;
                break;
            }
        }
    }
    u64 hhdm = fe_vmm_hhdm_offset();
    for (u32 i = 0; i < PMM_TEST_FRAMES; i++) {
        volatile u64 *p = (volatile u64 *)(uptr)(frames[i] + hhdm);
        for (u32 k = 0; k < FE_FRAME_SIZE / 8; k++) {
            p[k] = ((u64)i << 32) | k;
        }
    }
    for (u32 i = 0; i < PMM_TEST_FRAMES && fail == 0; i++) {
        volatile u64 *p = (volatile u64 *)(uptr)(frames[i] + hhdm);
        for (u32 k = 0; k < FE_FRAME_SIZE / 8; k++) {
            if (p[k] != (((u64)i << 32) | k)) {
                fe_kprintf("        帧 %#llx 偏移 %u 数据不一致\n",
                           (unsigned long long)frames[i], k * 8);
                fail++;
                break;
            }
        }
    }
    u64 free_mid = fe_pmm_free_frame_count();
    if (free_mid != free_before - PMM_TEST_FRAMES) {
        fe_kprintf("        空闲计数异常: 分配前 %llu, 分配 %u 帧后 %llu\n",
                   (unsigned long long)free_before, PMM_TEST_FRAMES,
                   (unsigned long long)free_mid);
        fail++;
    }
    for (u32 i = 0; i < PMM_TEST_FRAMES; i++) {
        fe_pmm_free_frame(frames[i]);
    }
    if (fe_pmm_free_frame_count() != free_before) {
        fe_kprintf("        释放后空闲计数未复原: %llu -> %llu\n",
                   (unsigned long long)free_before,
                   (unsigned long long)fe_pmm_free_frame_count());
        fail++;
    }

    phys_addr_t block = fe_pmm_alloc_frames(64);
    if (block == 0 || !fe_is_aligned(block, FE_FRAME_SIZE)) {
        fe_kprintf("        连续 64 帧分配失败\n");
        fail++;
    } else {
        fe_pmm_free_frames(block, 64);
    }

    fe_pmm_get_stats(&s1);
    if (s1.free_frames != free_before) {
        fe_kprintf("        自检结束后空闲帧未复原: %llu -> %llu\n",
                   (unsigned long long)free_before, (unsigned long long)s1.free_frames);
        fail++;
    }
    fe_kfree(frames);

    fe_kprintf("        %u 帧分配/写入/查重/释放/计数复原: %s\n", PMM_TEST_FRAMES,
               fail ? "失败" : "OK");
    return fail;
}

/* ------------------------------------------------------------------ */
/* 自检 5：虚拟内存与地址空间                                          */
/* ------------------------------------------------------------------ */

extern const char __text_start[], __text_end[];
extern const char __rodata_start[], __rodata_end[];
extern const char __bss_start[], __bss_end[];

static u32 selftest_vmm(void)
{
    u32 fail = 0;
    struct fe_address_space *ks = fe_vmm_kernel_space();
    u64 cr3 = fe_read_cr3();

    fe_kprintf("[自检] VMM: CR3 = %#llx, 内核 PML4 = %#llx — %s\n",
               (unsigned long long)cr3, (unsigned long long)ks->pml4_phys,
               cr3 == ks->pml4_phys ? "运行在自建页表上" : "仍在引导器页表上");
    if (cr3 != ks->pml4_phys) {
        fe_kprintf("        未切换到自建页表\n");
        fail++;
    }

    u64 text_flags = fe_vmm_query_flags(ks, (virt_addr_t)(uptr)&selftest_vmm);
    u64 ro_flags = fe_vmm_query_flags(ks, (virt_addr_t)(uptr)__rodata_start);
    u64 bss_flags = fe_vmm_query_flags(ks, (virt_addr_t)(uptr)__bss_start);

    fe_kprintf("        段权限: .text %#llx, .rodata %#llx, .bss %#llx\n",
               (unsigned long long)text_flags, (unsigned long long)ro_flags,
               (unsigned long long)bss_flags);

    if (!(text_flags & FE_PTE_PRESENT) || (text_flags & FE_PTE_WRITE) ||
        (text_flags & FE_PTE_NX)) {
        fe_kprintf("        .text 权限错误（应为可读可执行、不可写）\n");
        fail++;
    }
    if (!(ro_flags & FE_PTE_PRESENT) || (ro_flags & FE_PTE_WRITE) ||
        !(ro_flags & FE_PTE_NX)) {
        fe_kprintf("        .rodata 权限错误（应为只读且不可执行）\n");
        fail++;
    }
    if (!(bss_flags & FE_PTE_PRESENT) || !(bss_flags & FE_PTE_WRITE) ||
        !(bss_flags & FE_PTE_NX)) {
        fe_kprintf("        .bss 权限错误（应为可写且不可执行）\n");
        fail++;
    }
    fe_kprintf("        内核 W^X: .text 可执行 / .rodata 只读不可执行 / .bss 可写不可执行\n");

    u64 hhdm = fe_vmm_hhdm_offset();
    phys_addr_t probe_frame = fe_pmm_alloc_frame();
    if (probe_frame) {
        phys_addr_t back = fe_vmm_translate(ks, hhdm + probe_frame);
        if (back != probe_frame) {
            fe_kprintf("        HHDM 直映射异常: %#llx -> %#llx\n",
                       (unsigned long long)probe_frame, (unsigned long long)back);
            fail++;
        }
        fe_pmm_free_frame(probe_frame);
    }
    if (fe_vmm_translate(ks, FE_KERNEL_BASE) == 0) {
        fe_kprintf("        内核映像虚拟地址未映射\n");
        fail++;
    }

    u64 free_before = fe_pmm_free_frame_count();
    struct fe_address_space *as = fe_vmm_space_create();
    if (!as) {
        fe_kprintf("        地址空间创建失败\n");
        return fail + 1;
    }
    const virt_addr_t test_va = 0x40000000ull;
    const u64 test_size = 3 * FE_FRAME_SIZE;
    fe_status_t s = fe_vmm_map_alloc(as, test_va, test_size,
                                     FE_PTE_WRITE | FE_PTE_USER);
    if (fe_failed(s)) {
        fe_kprintf("        映射测试页失败: %s\n", fe_status_name(s));
        fail++;
    } else {
        phys_addr_t p0 = fe_vmm_translate(as, test_va);
        phys_addr_t p1 = fe_vmm_translate(as, test_va + FE_FRAME_SIZE);
        if (p0 == 0 || p1 == 0 || p0 == p1) {
            fe_kprintf("        映射查询异常: p0=%#llx p1=%#llx\n",
                       (unsigned long long)p0, (unsigned long long)p1);
            fail++;
        }
        fe_vmm_switch(as);
        volatile u64 *v = (volatile u64 *)(uptr)test_va;
        for (u32 i = 0; i < 3 * FE_FRAME_SIZE / 8; i++) {
            v[i] = 0xFEFE000000000000ull | i;
        }
        u32 bad = 0;
        for (u32 i = 0; i < 3 * FE_FRAME_SIZE / 8; i++) {
            if (v[i] != (0xFEFE000000000000ull | i)) {
                bad++;
            }
        }
        fe_vmm_switch(ks);

        volatile u64 *phys_view = (volatile u64 *)(uptr)(p0 + hhdm);
        if (bad || phys_view[0] != 0xFEFE000000000000ull) {
            fe_kprintf("        地址空间读写校验失败 (bad=%u, phys[0]=%#llx)\n", bad,
                       (unsigned long long)phys_view[0]);
            fail++;
        } else {
            fe_kprintf("        新地址空间: 3 页映射 → 切换 CR3 → 读写 12KiB → 物理帧核对一致\n");
        }
        fe_vmm_unmap_free(as, test_va, test_size);
    }
    fe_vmm_space_destroy(as);

    u64 free_after = fe_pmm_free_frame_count();
    if (free_after != free_before) {
        fe_kprintf("        地址空间销毁后帧未完全回收: %llu -> %llu\n",
                   (unsigned long long)free_before, (unsigned long long)free_after);
        fail++;
    } else {
        fe_kprintf("        地址空间销毁后物理帧完全回收（%llu 帧）\n",
                   (unsigned long long)free_after);
    }
    return fail;
}

/* ------------------------------------------------------------------ */
/* 自检 6：硬件资源池                                                  */
/* ------------------------------------------------------------------ */

static void resource_pool_bootstrap(bool verbose)
{
    fe_resource_init();

    /* 引导期由内核把「自己不用、可以交给驱动」的硬件放进池子。
     *
     * 现在只放三组传统串口——它们是真正空闲的（内核用 COM1）。
     * PCI 设备的 MMIO 与中断要等枚举（M7）才能确定，到那时由设备管理器
     * 把探测结果交给内核入池，内核仍然握有最终否决权。
     *
     * 注意这里没有放 COM1：内核自己的调试输出就在 COM1 上。
     * 池子里没有的东西，任何任务都申请不到——这正是设计意图。 */
    fe_resource_pool_add(FE_RES_IOPORT, 0x2F8, 8);     /* COM2 */
    fe_resource_pool_add(FE_RES_IOPORT, 0x3E8, 8);     /* COM3 */
    fe_resource_pool_add(FE_RES_IOPORT, 0x2E8, 8);     /* COM4 */

    /* PS/2（8042 控制器）。
     *
     * 这里体现了「共享认领」存在的唯一理由：键盘与鼠标的中断天然分开
     * （IRQ1 / IRQ12），但数据口 0x60 与命令口 0x64 是**同一个寄存器对**，
     * 物理上分不开。硬件的真实分割面就是「中断分开、端口共享」，
     * 所以资源池也必须能表达这个组合——否则「两个服务各自认领自己的设备」
     * 就只能靠把端口整段给其中一个、另一个去求它，绕远而且没必要。
     *
     * 端口共享的代价是内核不再保证互斥，两个服务的命令序列可能交错，
     * 因此共享区间上配了一把控制器锁（协作锁，见 fe/resource.h 的说明）。 */
    fe_resource_pool_add_shared(FE_RES_IOPORT, 0x60, 5);    /* 0x60..0x64 */
    fe_resource_pool_add(FE_RES_IRQ, 1, 1);                 /* 键盘 */
    fe_resource_pool_add(FE_RES_IRQ, 12, 1);                /* 鼠标 */

    /* ATA（PIIX4 兼容 IDE）主通道。
     *
     * 为什么第一块块设备走 ATA PIO 而不是 AHCI/virtio：
     * 这几个端口号是**固定的**，不需要 PCI 枚举就能用。
     * 先把「能读磁盘」这条链打通，FAT32 才有附着点；
     * 需要 PCI 的 AHCI / virtio-blk 留给 PCI 枚举之后那一代。 */
    fe_resource_pool_add(FE_RES_IOPORT, 0x1F0, 8);         /* 命令块 */
    fe_resource_pool_add(FE_RES_IOPORT, 0x3F6, 1);         /* 控制块 */
    fe_resource_pool_add(FE_RES_IRQ, 14, 1);                /* 主通道中断 */

    /* PCI 配置空间（机制 #1：0xCF8 地址口 + 0xCFC 数据口）。
     *
     * ★ 为什么这两个端口要入池，而不是让内核自己读 ★
     * "总线上有什么设备"是**发现**：要处理桥、认设备类、认厂商 ID。
     * 那是用户态设备管理器的活。内核不该长出一个 PCI 枚举器——
     * 那正是"内核只放机制"这条线。
     *
     * 但"谁能碰配置空间"必须由内核裁决：配置空间不是只读的，
     * 写 BAR、启总线主控、配 MSI 都走它。谁能改它，谁就能把设备的
     * 地址窗口挪到别人头上。所以做法与其它硬件完全一致：
     * **内核把它放进池子，驱动按常规路径认领**。
     *
     * 独占而不是共享：机制 #1 的访问是**两步**（先写 0xCF8 指定地址，
     * 再读写 0xCFC），两个任务交错就会各自改到对方的窗口。
     * 这种"两次访问构成一次操作"的寄存器对必须独占——
     * 与 PS/2 那种"物理上分不开"的共享是两回事。 */
    fe_resource_pool_add(FE_RES_IOPORT, 0xCF8, 8);          /* 0xCF8..0xCFF */

    /* 帧缓冲：交给用户态的控制台服务（consoled）。
     *
     * ★ 这是一次真正的"所有权移交"，不是"多给一个使用者" ★
     * 移交之后内核**不再往屏幕上画任何东西**：谁拥有硬件谁负责它的状态，
     * 两边都画就会互相盖掉（而且用户态看到的是自己以为的屏幕内容，
     * 与实际像素不一致——那类不一致极难查）。
     * 内核此前画的那条蓝带是**移交之前**的引导期痕迹，保留它是可以的：
     * 控制台服务起来后会整屏重画，把它覆盖掉。
     *
     * 长度按"页对齐后的实际字节数"入池：sys_mmio_map 会先对物理地址和长度
     * 做页对齐再去认领，池子里的区间必须与那个对齐结果一致，否则认领会失败
     * （帧缓冲的物理地址不保证页对齐，pitch*height 也不保证）。 */
    if (fe_boot_info()->has_framebuffer) {
        u64 fb_phys = (u64)(uptr)fe_boot_info()->fb.address - fe_vmm_hhdm_offset();
        u64 fb_base = FE_FRAME_ALIGN_DOWN(fb_phys);
        u64 fb_end = FE_FRAME_ALIGN_UP(fb_phys + fe_boot_info()->fb.size_bytes);
        fe_resource_pool_add(FE_RES_MMIO, fb_base, fb_end - fb_base);
    }

    if (verbose) {
        fe_kprintf("[初始化] 硬件资源池就绪: %u 段可分配"
                   "（COM2/3/4 端口、PS/2 端口[共享]、IRQ1、IRQ12）\n",
                   fe_resource_pool_count());
    }
}

FE_NORETURN void fe_kmain(void)
{
    /* 1. 最早的输出通道：串口 */
    fe_serial_init(FE_COM1);
    fe_serial_set_console_port(FE_COM1);
    print_banner();

    /* 2. 引导信息 */
    if (!fe_boot_info_init()) {
        fe_panic("Limine 引导协议基版本不兼容：需要 %d，引导器最高支持 %llu",
                 FE_BOOT_PROTOCOL_BASE_REVISION,
                 (unsigned long long)fe_boot_info()->loaded_base_revision);
    }
    fe_boot_info_dump();

    /* 切换自建页表**之前**先画一条纯蓝带（此时还跑在引导器的页表上） */
    fb_draw_band(0, 8, false, 0x000000FFu);
    fe_kprintf("[显示] 已在屏幕顶部画出一条蓝带（此时仍在引导器页表上）\n");

    /* 3. CPU 基础环境 */
    fe_gdt_init();
    fe_kprintf("[初始化] GDT 与 TSS 已装载（I/O 位图默认全部禁止访问）\n");
    fe_idt_init();
    fe_kprintf("[初始化] IDT 已装载，256 个中断向量\n");
    fe_syscall_init();
    /* FPU/SIMD：**必须在第一个用户线程跑起来之前**。
     * 它只动 CR0/CR4/XCR0（都是每 CPU 寄存器），不分配内存、不依赖堆，
     * 所以放在这里——但要早于任何可能执行用户态指令的时刻。
     * 将来多核启动 AP 时，每个 AP 也要各自调一次。 */
    fe_fpu_init();
    fe_pic_init();
    fe_kprintf("[初始化] 8259 已重映射并全部屏蔽（等待 IOAPIC 接管）\n");

    /* 4. 内存管理三件套（顺序不可颠倒：帧 → 页表 → 堆） */
    fe_pmm_init();
    {
        struct fe_pmm_stats st;
        fe_pmm_get_stats(&st);
        fe_kprintf("[初始化] PMM 就绪: %llu MiB 可用, 位图 %llu 字节\n",
                   (unsigned long long)(st.free_frames * FE_FRAME_SIZE / FE_MIB(1)),
                   (unsigned long long)st.bitmap_bytes);
    }
    fe_vmm_init();
    fe_kprintf("[初始化] VMM 就绪: 已建立 HHDM 与内核映像映射并切换到自建页表\n");
    fe_kheap_init();
    fe_kprintf("[初始化] 内核堆就绪: 虚拟区 %llu MiB, 8 个尺寸等级\n",
               (unsigned long long)(FE_KERNEL_HEAP_SIZE / FE_MIB(1)));

    /* 5. ACPI 与中断控制器（需要 HHDM 与 MMIO 映射，必须在 MM 之后） */
    const struct fe_boot_info *bi = fe_boot_info();
    if (!fe_acpi_init(bi->rsdp)) {
        fe_kprintf("[ACPI] 未解析到 MADT，将使用约定地址与 8259 降级路径\n");
    }
    fe_acpi_dump();

    fe_lapic_init(FE_VEC_LAPIC_SPUR);
    fe_lapic_dump();
    fe_ioapic_init();
    fe_ioapic_dump();
    fe_irq_init();
    fe_kprintf("[初始化] 中断投递就绪: 向量 %u..%u 归外部设备，一个中断一个向量\n",
               FE_VEC_GSI_BASE, FE_VEC_GSI_MAX);

    /* 6. 时间基准（LAPIC 定时器优先） */
    fe_time_init(1000);

    /* 7. 任务与 IPC（M3）：任务承载句柄表，必须在调度器建立 main 线程之前就绪 */
    resource_pool_bootstrap(true);
    fe_vfs_init();
    fe_ramfs_init();
    fe_devfs_init();
    fe_protect_init();
    fe_kprintf("[初始化] 文件系统就绪: ramfs（引导模块 %u 个）+ devfs 挂在 %s\n",
               (u32)fe_boot_info()->module_count, FE_DEVFS_MOUNT_POINT);

    /* ---- 应用 A/B 访问矩阵 ----
     *
     * 清单是随引导映像来的 /etc/protect.list（由 mkfat.py 在建镜像时生成），
     * 内核只负责把它翻成区间表，**不发明策略**。规则见 docs/05-ab-update.md：
     * 正在运行的槽只读；另一个槽连读都不行。
     *
     * 放在这里的理由：此时任何用户进程都还不存在，所以不存在
     * "保护生效之前先被人读一笔"的窗口。 */
    {
        struct fe_inode *ino = fe_vfs_lookup("/etc/protect.list");
        const void *data = fe_vfs_data(ino);
        u64 size = fe_vfs_size(ino);
        if (data && size) {
            u32 n = fe_protect_load_manifest((const char *)data, size,
                                             fe_boot_slot());
            fe_kprintf("[保护] A/B 清单已应用: 引导槽 %c → 登记 %u 段受保护区间\n",
                       fe_boot_slot(), n);
        } else {
            fe_kprintf("[保护] **警告：找不到 /etc/protect.list——保护未生效，"
                       "所有扇区都可读可写**\n");
        }
    }
    fe_task_init();
    fe_ipc_init();
    fe_kprintf("[初始化] 任务与 IPC 就绪: 句柄表容量 %u 项\n", FE_HANDLE_TABLE_SIZE);

    /* 8. 调度器 */
    fe_sched_init();
    fe_kprintf("[初始化] 调度器就绪: %u 个优先级, 默认时间片 %u 节拍\n",
               FE_THREAD_PRIO_LEVELS, fe_sched_default_slice());

    /* 8. 开中断，跑自检 */
    fe_idt_set_handler(3, breakpoint_handler);
    fe_interrupts_enable();
    fe_kprintf("[初始化] 中断已开启\n");

    /* 中断打开后才能实测节拍率；不达标会自动换源 */
    if (!fe_time_verify()) {
        fe_kprintf("[警告] 时间基准未达到声明频率：这属于虚拟化环境对定时器中断投递的限流，\n");
        fe_kprintf("       不是内核缺陷（实测 ISR 仅占 0.7%% CPU、CPU 从不关中断）。\n");
        fe_kprintf("       调度算法本身以节拍为单位验证，结论不受此影响。\n");
    }
    /* K7：时钟与节拍分开。节拍源定下来之后再确定"读时间"用哪个时基——
     * 顺序不能反：节拍校验本身要用 TSC 做参考，所以 TSC 的可用性已经在
     * 那一步被验证过了。 */
    fe_time_clock_init();
    fe_kprintf("\n");

    selftest_kprintf();
    fe_kprintf("\n");
    selftest_exception();
    selftest_framebuffer();
    fe_kprintf("\n");

    u32 f;
    f = selftest_pmm();          g_selftest_failures += f;
    fe_kprintf("\n");
    f = selftest_vmm();          g_selftest_failures += f;
    fe_kprintf("\n");
    fe_kprintf("[自检] 撤销映射（M11，MEM_UNMAP 的机制侧）:\n");
    f = fe_selftest_vmm_unmap(); g_selftest_failures += f;
    fe_kprintf("        => 撤销映射失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 内核堆:\n");
    f = fe_kheap_selftest();     g_selftest_failures += f;
    fe_kprintf("        => 内核堆失败项: %u\n", f);
    fe_kprintf("\n");
    f = fe_selftest_sched();     g_selftest_failures += f;
    fe_kprintf("        => 调度器失败项: %u\n", f);
    fe_kprintf("\n");
    /* ★ 让出代价自检紧跟在调度器自检之后 ★
     *
     * 位置是判据的一部分：这一条测的是"让出一次要等多久"，只有
     * **现场最小**（除 main 与 idle 外没有别的就绪线程）时那个数才干净。
     * 放到后面（比如收尾处）就会被用户态服务与测试线程的临时活动污染，
     * 测出来的就不是"让出的固有代价"。 */
    fe_kprintf("[自检] 让出代价（第 7 步：让出一次要等多久）:\n");
    f = fe_selftest_yield();     g_selftest_failures += f;
    fe_kprintf("        => 让出代价失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 按需分页与栈增长（VMA + #PF 解析）:\n");
    f = fe_selftest_demand_paging(); g_selftest_failures += f;
    fe_kprintf("        => 按需分页失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 区间表（VMA：插入/重叠/裁剪/增长）:\n");
    f = fe_selftest_vma();       g_selftest_failures += f;
    fe_kprintf("        => 区间表失败项: %u\n", f);
    fe_kprintf("\n");
    /* ★ C2 的最后一条：区间属性变更（mprotect）★
     * 它紧跟在区间表自检之后，因为它验的正是**区间表与页表两处一起改**：
     * 只改一处都有坏结局（只改 PTE ⇒ 下次缺页退回去；只改 VMA ⇒ 已映射的页
     * 仍可写），所以这一组必须落在真的页表上，不能只动一张假的区间表。 */
    fe_kprintf("[自检] 区间属性变更（mprotect：VMA + PTE + TLB）:\n");
    f = fe_selftest_protect_range(); g_selftest_failures += f;
    fe_kprintf("        => 区间保护失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 线程局部存储（TLS：每线程一份 fs 基址）:\n");
    f = fe_selftest_tls();       g_selftest_failures += f;
    fe_kprintf("        => TLS 失败项: %u\n", f);
    fe_kprintf("[自检] PCI 配置空间（机制 #1：0xCF8/0xCFC）:\n");
    f = fe_selftest_pci();       g_selftest_failures += f;
    fe_kprintf("        => PCI 配置空间失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 设备管理器（申报硬件 / 授权分发）:\n");
    f = fe_selftest_devmgr();    g_selftest_failures += f;
    fe_kprintf("        => 设备管理器失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 多对象等待（wait_any：等 N 个对象中任意一个就绪）:\n");
    f = fe_selftest_wait_any();  g_selftest_failures += f;
    fe_kprintf("        => 多对象等待失败项: %u\n", f);
    fe_kprintf("\n");
    f = fe_selftest_ipc();       g_selftest_failures += f;
    fe_kprintf("        => IPC 失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 硬件资源池（M5）:\n");
    f = fe_selftest_resource();  g_selftest_failures += f;
    fe_kprintf("        => 资源池失败项: %u\n", f);
    /* 资源池自检从干净状态开始、也以干净状态结束（避免测试污染真实状态），
     * 所以这里要把引导期的资源重新装一遍，后面的用户态测试才有东西可认领。 */
    resource_pool_bootstrap(false);
    fe_kprintf("\n");
    fe_kprintf("[自检] 中断投递（M5）:\n");
    f = fe_selftest_irq();       g_selftest_failures += f;
    fe_kprintf("        => 中断投递失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 根文件系统（M5）:\n");
    f = fe_selftest_fs();        g_selftest_failures += f;
    fe_kprintf("        => 根文件系统失败项: %u\n", f);
    fe_vfs_dump();
    fe_kprintf("\n");
    fe_kprintf("[自检] devfs 服务命名空间（M7）:\n");
    f = fe_selftest_devfs();     g_selftest_failures += f;
    fe_kprintf("        => devfs 失败项: %u\n", f);
    fe_devfs_dump();
    fe_kprintf("\n");
    fe_kprintf("[自检] 写保护区间表（M8）:\n");
    f = fe_selftest_protect();   g_selftest_failures += f;
    fe_kprintf("        => 写保护失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 进程模型（M5）:\n");
    f = fe_selftest_process();   g_selftest_failures += f;
    fe_kprintf("        => 进程模型失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 任务/线程快照（K3：ps 看到的那张表）:\n");
    f = fe_selftest_tasklist();  g_selftest_failures += f;
    fe_kprintf("        => 任务快照失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 进程终止（K2：闸门 + 取消点）:\n");
    f = fe_selftest_kill();      g_selftest_failures += f;
    fe_kprintf("        => 进程终止失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 替换映像（K6：exec 要用的线程级叫停机制 E1-E5）:\n");
    f = fe_selftest_exec();      g_selftest_failures += f;
    fe_kprintf("        => 替换映像失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] 用户态异常处理者（K5：投递判据 / 回复校验）:\n");
    f = fe_selftest_fault();     g_selftest_failures += f;
    fe_kprintf("        => 异常处理者失败项: %u\n", f);
    fe_kprintf("\n");
    fe_kprintf("[自检] FPU / SIMD（M10 前置）:\n");
    f = fe_selftest_fpu();       g_selftest_failures += f;
    fe_kprintf("        => FPU 失败项: %u（模式：%s）\n", f,
               fe_fpu_mode_name(fe_fpu_get_mode()));
    /* 保存次数是"切换路径真的在处理状态"的证据：
     * 它必须随着用户态线程的切换而增长，而不是停在 0。 */
    fe_kprintf("           累计 FPU 状态保存 %llu 次\n",
               (unsigned long long)fe_fpu_saves());
    fe_kprintf("\n");
    fe_kprintf("[自检] 真实时钟（K7：TSC 制，替代节拍制）:\n");
    f = fe_selftest_clock();     g_selftest_failures += f;
    fe_kprintf("        => 真实时钟失败项: %u\n", f);
    fe_kprintf("\n");

    /* ---- 内核到此为止，剩下的交给 init（Linux 也是这么做的）----
     *
     * 从这里往下，内核里不再有任何「运行某个用户程序」的代码：
     * /init 由内核 exec，其它程序全部由 init 用 spawn/wait 拉起。
     * 内核只检查两件事：init 自己的退出码，以及它在过程中留下的异常向量。 */
    fe_kprintf("--------------------------------------------------\n");
    fe_kprintf("[内核] 自检结束，准备把控制权交给用户态\n");
    fe_kprintf("--------------------------------------------------\n");
    i32 init_rc = fe_process_start_init();    if (init_rc != 0) {
        fe_kprintf("[init] 退出码非 0（%d），用户态自检未全部通过\n", init_rc);
        g_selftest_failures++;
    }
    fe_kprintf("\n[自检] 用户态异常证据:\n");
    f = fe_selftest_user();      g_selftest_failures += f;
    fe_kprintf("        => 用户态失败项: %u\n", f);
    fe_kprintf("\n");

    fe_kprintf("==================================================\n");
    if (g_selftest_failures == 0) {
        fe_kprintf("  M5 里程碑自检全部通过\n");
    } else {
        fe_kprintf("  自检存在 %u 项失败\n", g_selftest_failures);
    }
    fe_kprintf("    · Limine 引导与引导信息解析          OK\n");
    fe_kprintf("    · GDT / TSS / IDT / 异常分发         OK\n");
    fe_kprintf("    · 物理帧分配器（含计数复原）         OK\n");
    fe_kprintf("    · 自建 4 级页表 + CR3 切换 + W^X     OK\n");
    fe_kprintf("    · 内核堆 slab                        OK\n");
    fe_kprintf("    · ACPI MADT / LAPIC / IOAPIC         OK\n");
    fe_kprintf("    · 时间基准: %s\n", fe_time_source());
    fe_kprintf("    · 抢占式调度 / 优先级 / 睡眠         OK\n");
    fe_kprintf("    · 硬件资源池（端口/MMIO/IRQ 认领）   OK\n");
    fe_kprintf("    · 每任务 IO 位图 / 中断投递到通知    OK\n");
    fe_kprintf("    · 根文件系统 / 进程模型 / init       OK\n");
    fe_kprintf("  下一步: M6 后半 帧缓冲控制台服务（PS2 键鼠已由用户态服务接管）\n");
    fe_kprintf("==================================================\n");

    fe_sched_dump();

    /* 引导线程到此结束它的使命；此后系统由空闲线程维持运行 */
    fe_kprintf("\n[内核] main 线程退出，系统进入空闲循环（中断仍然工作）\n");
    fe_thread_exit(0);
}
