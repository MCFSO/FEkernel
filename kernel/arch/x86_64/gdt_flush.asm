; SPDX-License-Identifier: 0BSD
; GDT / TSS 装载桩。
;
; 这些指令序列属于 x86_64 硬件约定（lgdt/ltr 与远返回刷新 CS），
; 按 BSD/Linux 的既有惯例编写，不含任何内核逻辑。

bits 64
default rel

section .text

; void fe_gdt_load(const struct fe_gdtr *gdtr);
;   rdi = GDTR 指针
; 载入 GDT 后必须用远返回刷新 CS，否则 CS 仍指向旧描述符。
global fe_gdt_load
fe_gdt_load:
    lgdt [rdi]

    mov ax, 0x10            ; 内核数据段
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    pop rdi                 ; 取回返回地址
    mov rax, 0x08           ; 内核代码段
    push rax
    push rdi
    retfq                   ; 远返回：同时刷新 CS:RIP

; void fe_tss_load(void);
global fe_tss_load
fe_tss_load:
    mov ax, 0x28            ; TSS 选择子
    ltr ax
    ret
