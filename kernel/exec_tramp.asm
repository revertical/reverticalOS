; Run a 64-bit module on its own stack, then return to the loader.
; void rvmod_trampoline(u64 entry, u64 stack_top);

BITS 64
global rvmod_trampoline
section .text

rvmod_trampoline:
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    mov     rax, rdi               ; entry
    mov     rcx, rsi               ; stack top
    mov     rbx, rsp
    mov     rsp, rcx
    xor     rbp, rbp
    call    rax
    mov     rsp, rbx

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
