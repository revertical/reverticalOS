; ============================================================================
;  ReverticalOS kernel entry point (64-bit long mode)
; ============================================================================

BITS 64

global _start
extern kmain

section .entry
_start:
    cli
    mov     rsp, 0x90000
    xor     rbp, rbp
    mov     rdi, 0x7000           ; BootInfo produced by the bootloader
    call    kmain
.hang:
    cli
    hlt
    jmp     .hang

section .note.GNU-stack noalloc noexec nowrite progbits
