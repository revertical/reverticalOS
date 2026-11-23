; ============================================================================
;  ReverticalOS - Stage 1 boot sector
;  A hand-written BIOS boot sector. No GRUB, no Limine, no Linux.
;  Loads stage 2 at 0x7E00 with INT 13h LBA reads, then jumps to it.
; ============================================================================

BITS 16
ORG 0x7C00

STAGE2_LBA      equ 1
STAGE2_SECTORS  equ 16
BOOT_DRIVE      equ 0x0500

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti

    mov     [BOOT_DRIVE], dl        ; remember the BIOS boot drive

    ; ---- load stage 2 (LBA 1..16) at 0x7E00 -------------------------------
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [BOOT_DRIVE]
    int     0x13
    jc      boot_error

    mov     dl, [BOOT_DRIVE]
    jmp     0x0000:0x7E00

boot_error:
    mov     si, msg_error
    call    print
    mov     si, msg_reboot
    call    print
    jmp     $

print:
    mov     ah, 0x0E
.next:
    lodsb
    test    al, al
    jz      .done
    int     0x10
    jmp     .next
.done:
    ret

msg_error:  db "ReverticalOS: unable to read stage 2 from disk.", 13, 10, 0
msg_reboot: db "The system cannot boot. Press Ctrl+Alt+Del to restart.", 13, 10, 0

align 4
dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0x7E00
    dw 0x0000
    dq STAGE2_LBA

; ---------------------------------------------------------------------------
;  MBR partition table. tools/mkimage.py and the installer rewrite these four
;  entries; the boot code above never reads them, but the kernel uses them to
;  locate the RVFS volume.
; ---------------------------------------------------------------------------
times 446-($-$$) db 0
partition_table:
    times 64 db 0
dw 0xAA55
