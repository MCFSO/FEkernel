; SPDX-License-Identifier: 0BSD
; 用户程序入口。
;
; 内核按 Linux x86-64 的约定摆好初始栈：
;
;     rsp → argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL
;
; 所以这里的写法和 Linux 的 _start 一模一样：从 rsp 读 argc，紧接着就是 argv，
; 跳过 argv 的 NULL 终止符就是 envp。字符串本身也在栈上。
; main 的返回值就是进程退出码。
;
; ★ 这里额外做一件事：把 envp 交给 libposix ★
; 用户程序在 C 层拿不到 envp（main 的第三个参数是 glibc 的扩展，
; 而我们的 start 只传 argc/argv——与内核摆的栈一致）。
; POSIX 的 `environ` 需要一个来源，所以入口把它显式交过去。
; 顺序必须是「先保存原始 rsp → 再对齐 → 再取参数」：
; 对齐会改掉 rsp，`[rsp]` 就不再是 argc 了（这个坑在 C 层看不见，
; 但汇编里一旦顺序写错，main 收到的是垃圾 argc）。

bits 64
default rel

section .text
global _start
extern main
extern fe_exit
extern __posix_set_envp

_start:
    xor rbp, rbp                ; 终止栈回溯
    mov r12, rsp                ; ★ 先保存原始栈指针 ★
    mov rdi, [r12]              ; argc
    lea rsi, [r12 + 8]          ; argv
    lea rdx, [rsi + rdi*8 + 8]  ; envp = argv + argc*8 + 8（跳过 argv 的 NULL）
    and rsp, -16                ; SysV ABI：call 之前 rsp 必须 16 字节对齐

    mov rdi, rdx                ; 参数：envp
    call __posix_set_envp       ; 环境变量块交给 libposix（POSIX 的 environ）

    mov rdi, [r12]              ; argc（从保存的原始 rsp 重新读，不受对齐影响）
    lea rsi, [r12 + 8]          ; argv
    call main
    mov edi, eax                ; main 的返回值作为退出码
    call fe_exit
    hlt                         ; 理论上到不了
