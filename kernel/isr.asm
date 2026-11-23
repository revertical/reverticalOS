; ============================================================================
;  ReverticalOS exception stubs (64-bit long mode)
; ============================================================================

BITS 64
section .text

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push    qword 0
    push    qword %1
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push    qword %1
    jmp     isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

extern panic_exception

isr_common:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax

    mov     rdi, rsp
    and     rsp, -16                 ; SysV call alignment
    call    panic_exception

    ; panic_exception never returns
.hang:
    cli
    hlt
    jmp     .hang

section .note.GNU-stack noalloc noexec nowrite progbits
