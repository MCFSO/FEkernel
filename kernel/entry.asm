; SPDX-License-Identifier: 0BSD
; FEKernel 内核入口。
;
; Limine 把控制权交给 fe_entry 时的机器状态：
;   - long mode，分页已开启并已映射 HHDM 与内核映像
;   - 中断关闭，栈由引导器提供（我们不用它，立刻换到自己的栈）
;   - 所有 Limine 请求结构已被填好（见 include/fe/boot/limine.h）
;
; 这里只做「切换到自建栈 -> 调用 C 入口」，不含任何内核逻辑。

bits 64
default rel

section .bss
align 16
stack_bottom:
    resb 64 * 1024          ; 64 KiB 引导栈
global fe_boot_stack_top
fe_boot_stack_top:
stack_top:

section .text
global fe_entry
extern fe_kmain

fe_entry:
    cli
    cld

    mov rsp, stack_top      ; 换到内核自建栈（16 字节对齐）
    xor rbp, rbp            ; 终止栈回溯链，便于调试器展开

    call fe_kmain           ; 不返回

.halt:
    cli
    hlt
    jmp .halt
