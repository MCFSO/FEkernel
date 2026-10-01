; SPDX-License-Identifier: 0BSD
; FEKernel 系统调用入口。
;
; 与中断入口的区别：syscall 指令**不切换栈**，进入内核时 RSP 仍指向用户栈，
; 而且它会破坏 RCX（用户 RIP）与 R11（用户 RFLAGS）。因此入口必须先换栈，
; 再把用户现场伪造成与中断完全一致的栈帧，之后的路径就能与中断返回路径共用。
;
; 每个 CPU 有一个小结构体记录「当前线程的内核栈顶」与一个暂存槽：
;   gs:0 = 暂存用户 RSP
;   gs:8 = 当前线程内核栈顶（线程切换时由调度器更新）
; GS 基址在内核初始化时设置一次，用户态代码约定不使用 GS。
;
; 返回路径有两条：
;   - 本次是从 syscall 进来的（帧里 vector = 0x80）→ 用 sysretq 快速返回；
;   - 其它情况（线程第一次启动、被中断后恢复）→ 用 iretq。
; 二者共用同一套寄存器弹出序列，因此上下文切换对它们完全透明。

bits 64
default rel

%define USER_SS 0x1B          ; 用户数据段 0x18 | RPL 3
%define USER_CS 0x23          ; 用户代码段 0x20 | RPL 3
%define VEC_SYSCALL 0x80

section .text

extern fe_syscall_dispatch
extern fe_sched_maybe_switch

global fe_syscall_entry
fe_syscall_entry:
    mov [gs:0], rsp             ; 暂存用户栈指针
    mov rsp, [gs:8]             ; 切换到本线程的内核栈

    ; 构造与中断一致的栈帧（顺序：rip/cs/rflags/rsp/ss 由 iretq 弹出）
    push qword USER_SS
    push qword [gs:0]
    push r11                    ; 用户 RFLAGS（syscall 存进来的）
    push qword USER_CS
    push rcx                    ; 用户 RIP（syscall 存进来的）
    push qword 0                ; error
    push qword VEC_SYSCALL      ; vector

    ; 通用寄存器压栈顺序必须与 isr_common 一致（最终 r15 位于最低地址）
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

    cld

    mov rdi, rsp
    call fe_syscall_dispatch

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

    add rsp, 16                 ; 丢掉 vector 与 error
    cmp qword [rsp - 16], VEC_SYSCALL
    jne .return_iret

    ; ---- 用 sysret 快速返回 ----
    ; 注意：必须写成 `o64 sysret`。NASM 不把 `sysretq` 当指令，而是当成一个标签
    ; （只给 label-orphan 警告、不报错，于是**静默地不生成任何指令**）——踩过这个坑。
    mov rcx, [rsp]              ; 用户 RIP
    mov r11, [rsp + 16]         ; 用户 RFLAGS
    mov rsp, [rsp + 24]         ; 用户 RSP
    o64 sysret

.return_iret:
    iretq

; ------------------------------------------------------------------
; 设置 syscall/sysret 所需的 MSR。
;   STAR[47:32] = 内核 CS（syscall 进入时用），SS = CS+8
;   STAR[63:48] = 用户段基址，sysret 时 CS = 基址+16，SS = 基址+8
; 这正好对应 GDT 里刻意的排列：0x08 内核代码 / 0x10 内核数据 /
; 0x18 用户数据 / 0x20 用户代码。
; ------------------------------------------------------------------
global fe_syscall_msr_init
fe_syscall_msr_init:
    ; rdi = LSTAR（fe_syscall_entry 地址）
    mov rdx, rdi
    shr rdx, 32
    mov eax, edi
    mov ecx, 0xC0000082         ; IA32_LSTAR
    wrmsr

    ; STAR：注意 wrmsr 写入的是 **EDX:EAX 拼接而成的 64 位值**，
    ; 因此 64 位立即数必须显式拆成高 32 位（EDX）与低 32 位（EAX）。
    ; 曾经只把立即数放进 RAX 并清零 EDX，结果 STAR 被写成了 0 ——
    ; 症状是 sysret 加载出 CS=0x13/SS=0x0b 这种非法选择子，用户态一返回就 #GP。
    mov rax, 0x0010000800000000 ; STAR = (0x10 << 48) | (0x08 << 32)
    mov rdx, rax
    shr rdx, 32
    mov ecx, 0xC0000081
    wrmsr

    ; SFMASK：进入内核时清掉 IF/DF/TF，保证入口是原子的
    mov edx, 0
    mov eax, 0x700
    mov ecx, 0xC0000084
    wrmsr

    ; 打开 EFER.SCE
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1
    wrmsr

    ; GS 基址 = 每 CPU 暂存结构
    ; rsi = 结构地址
    mov rdx, rsi
    shr rdx, 32
    mov eax, esi
    mov ecx, 0xC0000101         ; IA32_GS_BASE
    wrmsr
    ret
