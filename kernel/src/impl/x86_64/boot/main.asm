global start
global stack_top
extern long_mode_start

section .text
bits 32
start:
    mov esp, stack_top

    push ebx             ; Save Multiboot Pointer
    call check_multiboot
    call check_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging
    call enable_sse
    pop edi
    lgdt [gdt64.pointer]
    jmp gdt64.code_segment:long_mode_start

    hlt

check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "M"
    jmp error

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "C"
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "L"
    jmp error

setup_page_tables:
    ; Zero out all tables
    mov edi, page_table_l4
    xor eax, eax
    mov ecx, 4096 * 6
    rep stosb

    ; Link L4 -> L3
    mov eax, page_table_l3
    or eax, 0b11
    mov [page_table_l4], eax
    mov [page_table_l4 + 256 * 8], eax

    ; Link L3 -> L2 (4 tables covering 0-4GB)
    mov eax, page_table_l2
    or eax, 0b11
    mov [page_table_l3], eax

    mov eax, page_table_l2 + 4096
    or eax, 0b11
    mov [page_table_l3 + 8], eax

    mov eax, page_table_l2 + (4096 * 2)
    or eax, 0b11
    mov [page_table_l3 + 16], eax

    mov eax, page_table_l2 + (4096 * 3)
    or eax, 0b11
    mov [page_table_l3 + 24], eax

    ; Identity map 4GB (2048 x 2MB huge pages)
    mov ecx, 0
.loop:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011      ; present, writable, huge page
    mov [page_table_l2 + ecx * 8], eax
    inc ecx
    cmp ecx, 2048
    jne .loop

    ret

enable_paging:
    mov eax, page_table_l4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5          ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8          ; long mode
    wrmsr

    mov eax, cr0
    or eax, 1 << 31         ; paging
    mov cr0, eax

    ret

enable_sse:
    mov eax, 0x1
    cpuid
    test edx, 1 << 25
    jz .no_sse

    ; FIX: use full 32-bit eax (not ax) so upper bits aren't garbage
    mov eax, cr0
    and eax, ~(1 << 2)      ; clear EM  (bit 2) — was: "and ax, 0xFFFB"
    or  eax,  (1 << 1)      ; set  MP   (bit 1) — was: "or  ax, 0x2"
    mov cr0, eax

    mov eax, cr4
    or  eax, (1 << 9)       ; OSFXSR
    or  eax, (1 << 10)      ; OSXMMEXCPT
    mov cr4, eax

    ret
.no_sse:
    mov al, "S"
    jmp error

error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt

section .bss
align 4096
page_table_l4:
    resb 4096
page_table_l3:
    resb 4096
page_table_l2:
    resb 4096 * 4
stack_bottom:
    resb 4096 * 4
stack_top:

section .rodata
gdt64:
    dq 0
.code_segment: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64