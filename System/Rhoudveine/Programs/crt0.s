.section .text
.global _start
_start:
    # Set up stack pointer if needed (kernel usually does this)
    # Call main
    call main
    # Call exit with return value of main
    mov %rax, %rdi
    call exit
    # Safety halt
1:  hlt
    jmp 1b

.global exit
exit:
    mov $0, %eax    # SYS_EXIT = 0
    syscall
    hlt
