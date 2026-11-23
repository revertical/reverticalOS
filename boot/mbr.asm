; ============================================================================
;  ReverticalOS - hybrid MBR boot stub
;
;  The ISO is both an El Torito CD and a USB disk image.  When the ISO is
;  written to a USB stick, the firmware sees the MBR partition table written
;  by tools/mkiso.py (an EFI System Partition holding BOOTX64.EFI), and a
;  legacy BIOS runs this stub from 0x7C00.
;
;  The stub chainloads the same 2048-byte El Torito stage 1 that a CD boot
;  uses (ISO LBA 20 = 512-byte LBA 80).  cdstage1 then detects that the BIOS
;  counts 512-byte sectors, loads the CD stage 2 and continues normally.
;
;  The first instructions run at 0x7C00 but are assembled for 0x0600, so the
;  sector is copied down before stage 1 is loaded over 0x7C00.
; ============================================================================

BITS 16
ORG 0x0600

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti
    cld

    ; Relocate to 0x0600; only absolute constants are used before this jump.
    mov     si, 0x7C00
    mov     di, 0x0600
    mov     cx, 256                 ; 512 bytes
    rep     movsw
    jmp     0x0000:relocated

relocated:
    mov     [boot_drive], dl
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    pushf
    xor     ax, ax                  ; some BIOSes clobber DS
    mov     ds, ax
    popf
    jc      .error

    mov     dl, [boot_drive]
    jmp     0x0000:0x7C00           ; stage 1 expects DL = drive

.error:
    mov     si, msg
    mov     ah, 0x0E
.next:
    lodsb
    test    al, al
    jz      .halt
    int     0x10
    jmp     .next
.halt:
    cli
    hlt
    jmp     .halt

align 4
boot_drive: db 0
dap:
    db 0x10, 0
    dw 4                            ; 4 x 512 = 2048 bytes
    dw 0x7C00
    dw 0x0000
    dq 80                           ; ISO LBA 20, in 512-byte sectors
msg: db "ReverticalOS: USB boot read error.", 13, 10, 0

; 0x000..0x1B7 code, 0x1B8 disk signature, 0x1BE partition table and 0x1FE
; signature are filled in by tools/mkiso.py.
times 440-($-$$) db 0
