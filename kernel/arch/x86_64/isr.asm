; SPDX-License-Identifier: 0BSD
; 中断/异常入口桩。
;
; 每个向量一个桩，作用是统一栈布局：
;   [vector][error][rip][cs][rflags][rsp][ss]
; 桩补上 vector（CPU 已压入 error 与后 5 项），随后跳转 isr_common，
; 保存通用寄存器并调用 C 的分发函数 fe_isr_dispatch(struct fe_regs *)。
;
; 压栈顺序必须与 include/fe/regs.h 的 struct fe_regs 完全一致。
; 本文件属于「按硬件约定编写的汇编桩」，遵循 BSD/Linux 的既有惯例。

bits 64
default rel

section .text

extern fe_isr_dispatch
extern fe_sched_maybe_switch

; 无错误码的向量：补一个 0 占位，保证栈布局一致
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; 有错误码的向量：CPU 已经压入错误码，只需压向量号
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common
%endmacro

; 硬件压入错误码的向量：
; 8(#DF) 10(#TS) 11(#NP) 12(#SS) 13(#GP) 14(#PF) 17(#AC) 21(#CP) 29(#VC) 30(#SX)
%assign vec 0
%rep 256
    %if vec = 8 || vec = 10 || vec = 11 || vec = 12 || vec = 13 || vec = 14 || vec = 17 || vec = 21 || vec = 29 || vec = 30
        ISR_ERRCODE vec
    %else
        ISR_NOERRCODE vec
    %endif
%assign vec vec+1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld                     ; 内核代码不假设方向标志状态

    mov rdi, rsp            ; 第一个参数 = struct fe_regs *
    call fe_isr_dispatch

    ; 中断返回前询问调度器是否需要切换线程。
    ; fe_sched_maybe_switch 返回「应当继续执行的栈指针」：
    ;   - 不需要切换时原样返回传入的 rsp；
    ;   - 需要切换时返回下一个线程保存的 rsp（其栈上同样躺着一个完整现场）。
    ; 于是抢占、主动让出、线程退出、线程首次启动这四种情形共用下面这一条返回路径。
    mov rdi, rsp
    call fe_sched_maybe_switch
    mov rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16             ; 丢弃 vector 与 error
    iretq

section .rodata
align 8
global fe_isr_stub_table
fe_isr_stub_table:
%assign v 0
%rep 256
    dq isr %+ v
%assign v v+1
%endrep
