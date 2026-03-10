; syscall.asm - SYSCALL instruction entry point
; This is called when userspace executes the SYSCALL instruction

section .text
global syscall_entry
extern syscall_handler

; SYSCALL calling convention:
; RCX = return address (saved by CPU)
; R11 = saved RFLAGS (saved by CPU)
; RAX = syscall number
; RDI = arg1
; RSI = arg2
; RDX = arg3
; R10 = arg4 (RCX is clobbered by SYSCALL, so arg4 uses R10)
; R8  = arg5
; R9  = arg6

syscall_entry:
    ; SYSCALL instruction clobbers RCX (return address) and R11 (RFLAGS)
    ; We must save them. We also need to save all caller-saved registers
    ; because the SysV ABI doesn't guarantee their preservation across calls.
    ; However, we also need to save callee-saved registers to be safe during context switches.

    ; 1. Save user stack pointer and swap to kernel stack
    ; We use R12 as a temporary but MUST save it first if we want to be clean.
    ; Actually, saving everything on the kernel stack is better.
    
    ; FOR NOW: Use a static kernel stack. 
    ; In a multi-threaded OS, we'd use swapgs to get per-CPU data (TSS/Stack).
    
    mov [rel temp_rsp_storage], rsp
    lea rsp, [rel kernel_syscall_stack_top]

    ; Push all registers to create a full interrupt-like frame
    push qword [rel temp_rsp_storage] ; User RSP
    push r11                          ; User RFLAGS
    push rcx                          ; User RIP (return address)
    
    push rax
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9
    
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Set up arguments for syscall_handler(num, arg1, arg2, arg3, arg4, arg5)
    ; SYSCALL convention: RAX=num, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5, R9=a6
    ; System V C convention: RDI, RSI, RDX, RCX, R8, R9
    
    mov r9, r8      ; arg5 -> r9
    mov r8, r10     ; arg4 -> r8
    mov rcx, rdx    ; arg3 -> rcx
    mov rdx, rsi    ; arg2 -> rdx
    mov rsi, rdi    ; arg1 -> rsi
    mov rdi, rax    ; num -> rdi
    
    call syscall_handler
    ; Return value is in RAX
    
    ; Restore registers (inverse order)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    ; Don't pop RAX, it holds the return value
    add rsp, 8
    
    pop rcx ; Restore RIP
    pop r11 ; Restore RFLAGS
    pop rsp ; Restore user RSP
    
    o64 sysret

section .bss
    resb 8192
kernel_syscall_stack_top:
    temp_rsp_storage: resq 1
