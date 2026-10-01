/* SPDX-License-Identifier: 0BSD */
/* FEKernel 内核链接脚本
 *
 * 内核被放在地址空间最顶端 2GiB 内，因此可以用 -mcmodel=kernel 生成
 * 更紧凑的代码（这条也是 Limine 引导协议的要求：内核必须位于 top 2GiB）。
 *
 * 四个 PT_LOAD 段分开声明，以便引导器给出正确的 MMU 权限：
 *   limine_requests 必须可写（引导器要在其中写入响应指针）
 *   text 可执行只读、rodata 只读、data 可写
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(fe_entry)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;

    /* Limine 请求区：引导器扫描 START/END 标记之间的内容 */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        KEEP(*(.fe_font))
        __rodata_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.llvm_addrsig)
    }
}
