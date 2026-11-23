#ifndef RVOS_PANIC_H
#define RVOS_PANIC_H

#include "types.h"
#include "idt.h"

/* Stop the system, log the reason and paint the panic screen.  Never returns.
   Modules reach this through the exported rv_panic() API. */
void panic(const char *reason) __attribute__((noreturn));

/* Called by the assembly exception stubs through idt.c. */
void panic_exception(const ExceptionFrame *frame);

#endif
