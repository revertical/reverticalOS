OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(16) {
        *(.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
        *(.assets)
    }

    . = ALIGN(0x1000);
    __image_end = .;

    /* Zero-initialised data lives at 1 MiB. The bootloader only loads the
       bytes above; the kernel clears this range during early init. */
    . = 0x300000;
    __bss_start = .;
    .bss (NOLOAD) : ALIGN(0x1000) {
        *(.bss*)
        *(COMMON)
    }
    __bss_end = ALIGN(0x1000);

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
        *(.gnu*)
    }
}
