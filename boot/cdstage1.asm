; ============================================================================
;  ReverticalOS - El Torito stage 1 (no-emulation CD boot)
;
;  The BIOS loads this 2048-byte image at 0x7C00 and passes the CD drive
;  number in DL. It reads stage 2 (8 KiB, 4 x 2048-byte virtual sectors)
;  from the fixed ISO location and jumps to 0x7E00.
;
;  El Torito no-emulation reads use 2048-byte sectors on a real CD. Some
;  BIOSes instead count 512-byte sectors, so the loader verifies a magic
;  signature at the end of the stage 2 image and retries with 512-byte
;  addressing when needed.
; ============================================================================

BITS 16
ORG 0x7C00

BOOT_DRIVE      equ 0x0500
SECTOR_MODE     equ 0x0504          ; 0 = 2048-byte CD sectors, 1 = 512
STAGE2_LBA_CD   equ 21              ; 2048-byte units
STAGE2_LBA_512  equ 84              ; 21 * 2048 / 512
SIG_ADDR        equ 0x9DF8          ; 0x7E00 + 8184 (last 8 bytes of stage 2)

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti
    cld

    mov     [BOOT_DRIVE], dl
    mov     byte [SECTOR_MODE], 0

    ; ---- first attempt: CD virtual sectors (2048 bytes) -------------------
    mov     word [dap_count], 4
    mov     dword [dap_lba], STAGE2_LBA_CD
    call    read_stage2
    jc      .try_512
    call    check_signature
    jnc     .boot

.try_512:
    ; ---- fallback: BIOS counts 512-byte sectors --------------------------
    mov     word [dap_count], 16
    mov     dword [dap_lba], STAGE2_LBA_512
    call    read_stage2
    jc      boot_error
    call    check_signature
    jc      boot_error
    mov     byte [SECTOR_MODE], 1

.boot:
    mov     dl, [BOOT_DRIVE]
    jmp     0x0000:0x7E00

; ---------------------------------------------------------------------------
;  read_stage2: transfer dap_count sectors to 0:7E00, with three retries.
;  Returns carry set on failure.
; ---------------------------------------------------------------------------
read_stage2:
    mov     byte [retries], 3
.retry:
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [BOOT_DRIVE]
    int     0x13
    pushf
    xor     ax, ax                   ; the BIOS may clobber DS/ES
    mov     ds, ax
    mov     es, ax
    popf
    jnc     .done

    xor     ah, ah
    mov     dl, [BOOT_DRIVE]
    int     0x13                     ; reset the drive
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    dec     byte [retries]
    jnz     .retry
    stc
    ret
.done:
    clc
    ret

; ---------------------------------------------------------------------------
;  check_signature: verify "RVSTG2CD" at the end of the loaded stage 2.
; ---------------------------------------------------------------------------
check_signature:
    cmp     dword [SIG_ADDR], 'RVST'
    jne     .bad
    cmp     dword [SIG_ADDR+4], 'G2CD'
    jne     .bad
    clc
    ret
.bad:
    stc
    ret

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

msg_error:  db "ReverticalOS: unable to read the CD boot image.", 13, 10, 0
msg_reboot: db "Press Ctrl+Alt+Del to restart.", 13, 10, 0

align 4
dap:
    db 0x10, 0
dap_count:  dw 0
dap_off:    dw 0x7E00
dap_seg:    dw 0x0000
dap_lba:    dq 0

retries:    db 0

times 2048-($-$$) db 0
