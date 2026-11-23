#ifndef RVOS_IDT_H
#define RVOS_IDT_H

#include "types.h"

/* Frame built by kernel/idt.asm and passed to panic_exception(). */
typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 vector, error, eip, cs, eflags;
} ExceptionFrame;

void idt_init(void);

#endif
