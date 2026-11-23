; ============================================================================
;  ReverticalOS - Stage 2 bootloader
;  - enables A20
;  - probes VESA VBE for the best 32bpp linear framebuffer mode
;  - loads the ReverticalOS kernel (flat binary) from disk with INT 13h LBA
;  - sets up a flat GDT and enters 32-bit protected mode
; ============================================================================

BITS 16
ORG 0x7E00

BOOT_DRIVE      equ 0x0500
VBE_INFO        equ 0x4000
MODE_INFO       equ 0x4200
MODE_LIST       equ 0x4400
KERNEL_HDR      equ 0x4C00
KERNEL_LOAD_SEG equ 0x1000          ; BIOS bounce buffer, linear 0x10000
KERNEL_DEST     equ 0x100000        ; kernel is linked at 1 MiB
KERNEL_MAX      equ 0x180000        ; 1.5 MiB, below BSS at 3 MiB

%ifdef CDBOOT
; El Torito no-emulation: the ISO is addressed in 2048-byte blocks.
; cdstage1 records at 0x0504 whether the BIOS actually counted 512-byte
; sectors, in which case the LBA values below are multiplied by four.
KERNEL_LBA      equ 25              ; kernel header block
KERNEL_DATA_LBA equ 26              ; kernel data block
KERNEL_LBA_512  equ 100             ; same byte offset, 512-byte units
KERNEL_DATA_LBA_512 equ 104
SECTOR_SHIFT    equ 11              ; 2048 bytes per block
CHUNK_SECTORS   equ 8               ; 8 blocks = 16 KiB per BIOS read
SEG_ADVANCE     equ 7               ; 2048 bytes / 16 = 128 paragraphs
%else
KERNEL_LBA      equ 17              ; header sector
KERNEL_DATA_LBA equ 18
SECTOR_SHIFT    equ 9               ; 512 bytes per sector
CHUNK_SECTORS   equ 32              ; 32 sectors = 16 KiB per BIOS read
SEG_ADVANCE     equ 5               ; 512 bytes / 16 = 32 paragraphs
%endif

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti
    cld

    call    serial_init
    mov     si, msg_banner
    call    serial_puts

    ; ---------------------------------------------------------------- A20 ---
    call    enable_a20

    ; ----------------------------------------------------------- VBE probe ---
    call    probe_video

    ; -------------------------------------------------------- load kernel ---
    ; Unreal mode gives FS a 4 GiB flat limit so each BIOS read can be
    ; copied from the 64 KiB bounce buffer up to the kernel link address.
    call    enter_unreal
    call    load_kernel

    ; Show a short summary in the BIOS text screen before switching modes.
    mov     si, msg_loading
    call    print

    mov     ax, [best_w]
    call    print_dec
    mov     si, msg_x
    call    print
    mov     ax, [best_h]
    call    print_dec
    mov     si, msg_bpp
    call    print

    ; ------------------------------------------------------------ set mode ---
    call    set_video_mode

    ; ---------------------------------------------------------- boot info ----
    call    fill_bootinfo

    mov     si, msg_pm
    call    serial_puts

    ; ------------------------------------------------------- protected mode --
    cli
    lgdt    [gdt_desc]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     0x08:pm_entry

; ============================================================================
;  Unreal mode: FS keeps a flat 4 GiB limit in real mode for high copies
; ============================================================================
enter_unreal:
    cli
    lgdt    [gdt_desc]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     0x08:unreal_pm
BITS 32
unreal_pm:
    mov     ax, 0x10
    mov     fs, ax
    mov     gs, ax
    mov     eax, cr0
    and     eax, ~1
    mov     cr0, eax
    jmp     0x0000:unreal_rm
BITS 16
unreal_rm:
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    sti
    ret

; ============================================================================
;  A20
; ============================================================================
enable_a20:
    mov     ax, 0x2401
    int     0x15
    in      al, 0x92
    test    al, 2
    jnz     .done
    or      al, 2
    and     al, 0xFE
    out     0x92, al
.done:
    ret

; ============================================================================
;  VBE mode probing
; ============================================================================
probe_video:
    mov     si, msg_vbe
    call    serial_puts

    mov     dword [VBE_INFO], 'VBE2'
    xor     ax, ax
    mov     es, ax
    mov     di, VBE_INFO
    mov     ax, 0x4F00
    int     0x10
    cmp     ax, 0x004F
    jne     .fail
    cmp     dword [VBE_INFO], 'VESA'
    jne     .fail
    mov     si, msg_vbeok
    call    serial_puts

    ; Copy the mode list to low memory so it stays reachable.
    mov     si, [VBE_INFO+0x0E]
    mov     ax, [VBE_INFO+0x10]
    mov     ds, ax
    xor     ax, ax
    mov     es, ax
    mov     di, MODE_LIST
    mov     cx, 1024
.copy:
    lodsw
    stosw
    cmp     ax, 0xFFFF
    je      .copied
    loop    .copy
.copied:
    xor     ax, ax
    mov     ds, ax
    mov     si, msg_vbelist
    call    serial_puts

    mov     dword [best_score], 0
    mov     word [best_mode], 0xFFFF
    mov     word [mode_ptr], MODE_LIST
    mov     word [modes_seen], 0
.next_mode:
    mov     si, [mode_ptr]
    mov     ax, [si]
    cmp     ax, 0xFFFF
    je      .selected
    add     word [mode_ptr], 2
    inc     word [modes_seen]
    mov     [cur_mode], ax

    mov     al, '.'
    call    serial_putc

    xor     ax, ax
    mov     es, ax
    mov     di, MODE_INFO
    mov     cx, [cur_mode]
    mov     ax, 0x4F01
    int     0x10
    push    ax
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    pop     ax
    cmp     ax, 0x004F
    jne     .next_mode

    ; supported + graphics + linear framebuffer
    mov     ax, [MODE_INFO]
    and     ax, 0x0091
    cmp     ax, 0x0091
    jne     .next_mode
    cmp     byte [MODE_INFO+0x19], 32
    jne     .next_mode

    ; debug: dump every 32bpp linear mode
    mov     si, msg_dbgmode
    call    serial_puts
    mov     ax, [cur_mode]
    call    serial_hex16
    mov     si, msg_space
    call    serial_puts
    mov     ax, [MODE_INFO+0x12]
    call    serial_dec16
    mov     si, msg_x
    call    serial_puts
    mov     ax, [MODE_INFO+0x14]
    call    serial_dec16
    mov     si, msg_nl
    call    serial_puts

    movzx   eax, word [MODE_INFO+0x12]
    movzx   ebx, word [MODE_INFO+0x14]
    cmp     eax, 1024
    jb      .next_mode
    cmp     ebx, 600
    jb      .next_mode
    cmp     eax, 1680
    ja      .next_mode
    cmp     ebx, 1100
    ja      .next_mode

    ; require a sane aspect ratio (1.20 .. 1.85)
    mov     ecx, eax
    imul    ecx, 100
    mov     eax, ecx
    xor     edx, edx
    div     ebx
    cmp     eax, 120
    jb      .next_mode
    cmp     eax, 185
    ja      .next_mode

    ; score = area (+ bonus for the intended 1280x1024 canvas)
    movzx   eax, word [MODE_INFO+0x12]
    movzx   ebx, word [MODE_INFO+0x14]
    imul    eax, ebx
    cmp     word [MODE_INFO+0x12], 1280
    jne     .score
    cmp     word [MODE_INFO+0x14], 1024
    jne     .score
    add     eax, 20000000
.score:
    cmp     eax, [best_score]
    jbe     .next_mode
    mov     [best_score], eax
    mov     ax, [cur_mode]
    mov     [best_mode], ax
    mov     ax, [MODE_INFO+0x12]
    mov     [best_w], ax
    mov     ax, [MODE_INFO+0x14]
    mov     [best_h], ax
    mov     ax, [MODE_INFO+0x10]
    mov     [best_pitch], ax
    mov     eax, [MODE_INFO+0x28]
    mov     [best_phys], eax
    jmp     .next_mode

.selected:
    mov     si, msg_nl
    call    serial_puts
    cmp     word [best_mode], 0xFFFF
    je      .fallback

    ; debug: print chosen mode
    mov     si, msg_mode
    call    serial_puts
    mov     ax, [best_mode]
    call    serial_hex16
    mov     si, msg_space
    call    serial_puts
    mov     ax, [best_w]
    call    serial_dec16
    mov     si, msg_x
    call    serial_puts
    mov     ax, [best_h]
    call    serial_dec16
    mov     si, msg_nl
    call    serial_puts
    ret

.fallback:
    ; last resort: VESA 1024x768x32
    mov     word [best_mode], 0x0118
    mov     word [best_w], 1024
    mov     word [best_h], 768
    mov     word [best_pitch], 4096
    mov     si, msg_fallback
    call    serial_puts
    ret
.fail:
    mov     si, msg_novbe
    call    serial_puts
    jmp     halt_error

; ============================================================================
;  Load kernel (flat binary) to 0x10000
; ============================================================================
load_kernel:
    mov     si, msg_load
    call    serial_puts

    xor     ax, ax
    mov     ds, ax

%ifdef CDBOOT
    ; Choose the CD addressing mode detected by cdstage1.
    cmp     byte [0x0504], 0
    je      .cd_2048
    mov     byte [boot_sector_shift], 9
    mov     word [boot_chunk_max], 32
    mov     byte [boot_seg_advance], 5
    mov     dword [boot_kernel_lba], KERNEL_LBA_512
    mov     dword [boot_kernel_data], KERNEL_DATA_LBA_512
.cd_2048:
%endif

    ; header sector
%ifdef CDBOOT
    mov     eax, [boot_kernel_lba]
    mov     [dap_lba], eax
%else
    mov     dword [dap_lba], KERNEL_LBA
%endif
    mov     word [dap_count], 1
    mov     word [dap_off], 0
    mov     word [dap_seg], KERNEL_HDR >> 4
    mov     si, dap_load
    mov     ah, 0x42
    mov     dl, [BOOT_DRIVE]
    int     0x13
    jc      .io_error

    ; Read the kernel header before touching the high-memory copy pointer.

    cmp     dword [KERNEL_HDR], 'RVK1'
    jne     .bad_header
    mov     eax, [KERNEL_HDR+4]
    cmp     eax, 512
    jb      .bad_header
    cmp     eax, KERNEL_MAX
    ja      .bad_header
    mov     [kernel_size], eax
    mov     dword [kernel_dest], KERNEL_DEST

    ; sectors = ceil(size / sector_size)
%ifdef CDBOOT
    mov     cl, [boot_sector_shift]
    mov     edx, 1
    shl     edx, cl
    dec     edx
    add     eax, edx
    shr     eax, cl
%else
    add     eax, (1 << SECTOR_SHIFT) - 1
    shr     eax, SECTOR_SHIFT
%endif
    mov     [sectors_left], eax

%ifdef CDBOOT
    mov     eax, [boot_kernel_data]
    mov     [dap_lba], eax
%else
    mov     dword [dap_lba], KERNEL_DATA_LBA
%endif
    mov     word [dap_seg], KERNEL_LOAD_SEG
    mov     word [dap_off], 0

.read_loop:
    mov     eax, [sectors_left]
    test    eax, eax
    jz      .loaded

    xor     ecx, ecx
%ifdef CDBOOT
    mov     cx, [boot_chunk_max]         ; at most 16 KiB per transfer
%else
    mov     cx, CHUNK_SECTORS            ; at most 16 KiB per transfer
%endif
    cmp     eax, ecx
    jae     .count_ok
    mov     cx, ax
.count_ok:
    mov     [dap_count], cx
    mov     [chunk_sectors], cx

    mov     byte [retries], 3
.try_read:
    mov     si, dap_load
    mov     ah, 0x42
    mov     dl, [BOOT_DRIVE]
    int     0x13
    pushf
    xor     ax, ax                       ; BIOS may clobber DS
    mov     ds, ax
    popf
    jnc     .read_ok
    xor     ah, ah
    mov     dl, [BOOT_DRIVE]
    int     0x13                         ; reset disk
    xor     ax, ax
    mov     ds, ax
    dec     byte [retries]
    jnz     .try_read
    jmp     .io_error
.read_ok:
    ; Copy this chunk from the 0x10000 bounce buffer to the kernel image.
    movzx   eax, word [chunk_sectors]
%ifdef CDBOOT
    movzx   edx, byte [boot_sector_shift]
    mov     ecx, edx
    mov     edx, eax
    shl     edx, cl
%else
    mov     edx, eax
    shl     edx, 9
%endif
    mov     esi, 0x10000
    mov     edi, [kernel_dest]
    mov     ecx, edx
.copy_chunk:
    ; GS keeps its flat 4 GiB limit in unreal mode too, so both the low
    ; bounce buffer (GS:ESI) and the high kernel image (FS:EDI) are
    ; reachable from real mode.
    mov     al, [gs:esi]
    mov     [fs:edi], al
    inc     esi
    inc     edi
    dec     ecx
    jnz     .copy_chunk
    add     [kernel_dest], edx

    ; The bounce buffer is reused for every chunk; only the high-memory
    ; destination advances.
    mov     word [dap_seg], KERNEL_LOAD_SEG
    movzx   eax, word [chunk_sectors]
    add     [dap_lba], eax
    mov     eax, [sectors_left]
    movzx   ebx, word [chunk_sectors]
    sub     eax, ebx
    mov     [sectors_left], eax

    mov     al, '.'
    call    serial_putc
    jmp     .read_loop

.loaded:
    mov     si, msg_nl
    call    serial_puts
    ret

.io_error:
    mov     si, msg_diskerr
    call    serial_puts
    jmp     halt_error
.bad_header:
    mov     si, msg_badhdr
    call    serial_puts
    jmp     halt_error

; ============================================================================
;  Set video mode
; ============================================================================
set_video_mode:
    mov     ax, 0x4F02
    mov     bx, [best_mode]
    or      bx, 0x4000
    xor     di, di
    int     0x10
    push    ax
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    pop     ax
    cmp     ax, 0x004F
    jne     .failed

    ; refresh mode info for the actual pitch / framebuffer
    xor     ax, ax
    mov     es, ax
    mov     di, MODE_INFO
    mov     cx, [best_mode]
    mov     ax, 0x4F01
    int     0x10
    push    ax
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    pop     ax
    cmp     ax, 0x004F
    jne     .failed
    mov     ax, [MODE_INFO+0x12]
    mov     [best_w], ax
    mov     ax, [MODE_INFO+0x14]
    mov     [best_h], ax
    mov     ax, [MODE_INFO+0x10]
    test    ax, ax
    jnz     .pitch_ok
    mov     ax, [best_w]
    shl     ax, 2
.pitch_ok:
    mov     [best_pitch], ax
    mov     eax, [MODE_INFO+0x28]
    mov     [best_phys], eax
    ret

.failed:
    ; Roll back to plain VGA text mode and report.
    mov     ax, 0x0003
    int     0x10
    mov     si, msg_vidfail
    call    print
    jmp     halt_error

; ============================================================================
;  Boot info structure at 0x7000
; ============================================================================
fill_bootinfo:
    mov     dword [0x7000], 'RVBO'
    mov     eax, [best_phys]
    mov     [0x7004], eax
    mov     dword [0x7008], 0          ; framebuffer above 4 GiB (never for VBE)
    movzx   eax, word [best_w]
    mov     [0x700C], eax
    movzx   eax, word [best_h]
    mov     [0x7010], eax
    movzx   eax, word [best_pitch]
    mov     [0x7014], eax
    movzx   eax, byte [MODE_INFO+0x19]
    mov     [0x7018], eax
    mov     ax, [best_mode]
    mov     [0x701C], ax
    mov     al, [MODE_INFO+0x20]
    mov     [0x701E], al
    mov     al, [MODE_INFO+0x1F]
    mov     [0x701F], al
    mov     al, [MODE_INFO+0x22]
    mov     [0x7020], al
    mov     al, [MODE_INFO+0x21]
    mov     [0x7021], al
    mov     al, [MODE_INFO+0x24]
    mov     [0x7022], al
    mov     al, [MODE_INFO+0x23]
    mov     [0x7023], al
    mov     eax, [kernel_size]
    mov     [0x7024], eax
%ifdef CDBOOT
    ; Booting from CD: no attached disk is the running system. The installer
    ; uses this sentinel to avoid tagging a target disk as "RUNNING".
    mov     dword [0x7028], 0xFFFFFFFF
%else
    xor     eax, eax
    mov     al, [BOOT_DRIVE]
    mov     [0x7028], eax
%endif
    mov     dword [0x702C], 0           ; ACPI RSDP low
    mov     dword [0x7030], 0           ; ACPI RSDP high
    mov     dword [0x7034], 0           ; flags
    mov     dword [0x7038], 0           ; reserved
    ret

; ============================================================================
;  BIOS text helpers
; ============================================================================
print:
    mov     ah, 0x0E
.loop:
    lodsb
    test    al, al
    jz      .done
    int     0x10
    jmp     .loop
.done:
    ret

print_dec:
    push    ax
    push    bx
    push    cx
    push    dx
    mov     bx, 10
    xor     cx, cx
.div:
    xor     dx, dx
    div     bx
    push    dx
    inc     cx
    test    ax, ax
    jnz     .div
.out:
    pop     dx
    mov     al, dl
    add     al, '0'
    mov     ah, 0x0E
    int     0x10
    loop    .out
    pop     dx
    pop     cx
    pop     bx
    pop     ax
    ret

; ============================================================================
;  Serial helpers (COM1 0x3F8)
; ============================================================================
serial_init:
    mov     dx, 0x3F9
    xor     al, al
    out     dx, al
    mov     dx, 0x3FB
    mov     al, 0x80
    out     dx, al
    mov     dx, 0x3F8
    mov     al, 1
    out     dx, al
    mov     dx, 0x3F9
    xor     al, al
    out     dx, al
    mov     dx, 0x3FB
    mov     al, 3
    out     dx, al
    mov     dx, 0x3FA
    mov     al, 0xC7
    out     dx, al
    mov     dx, 0x3FC
    mov     al, 0x0B
    out     dx, al
    ret

serial_putc:
    push    ax
    push    dx
    mov     ah, al
.wait_tx:
    mov     dx, 0x3FD
    in      al, dx
    test    al, 0x20
    jz      .wait_tx
    mov     dx, 0x3F8
    mov     al, ah
    out     dx, al
    pop     dx
    pop     ax
    ret

serial_puts:
    push    ax
.loop:
    lodsb
    test    al, al
    jz      .done
    call    serial_putc
    jmp     .loop
.done:
    pop     ax
    ret

serial_hex16:
    push    ax
    mov     al, ah
    call    .byte
    pop     ax
    call    .byte
    ret
.byte:
    push    ax
    shr     al, 4
    call    .nibble
    pop     ax
    and     al, 0x0F
.nibble:
    cmp     al, 10
    jb      .digit
    add     al, 'A'-10
    jmp     .out
.digit:
    add     al, '0'
.out:
    call    serial_putc
    ret

serial_dec16:
    push    ax
    push    bx
    push    cx
    push    dx
    mov     bx, 10
    xor     cx, cx
.div:
    xor     dx, dx
    div     bx
    push    dx
    inc     cx
    test    ax, ax
    jnz     .div
.out:
    pop     dx
    mov     al, dl
    add     al, '0'
    call    serial_putc
    loop    .out
    pop     dx
    pop     cx
    pop     bx
    pop     ax
    ret

halt_error:
    cli
    mov     si, msg_halt
    call    print
.h:
    hlt
    jmp     .h

; ============================================================================
;  Data
; ============================================================================
msg_banner:    db 13, 10, "ReverticalOS stage2 bootloader", 13, 10, 0
msg_vbe:       db "Probing VESA VBE modes...", 13, 10, 0
msg_vbeok:     db "VBE controller OK.", 13, 10, 0
msg_vbelist:   db "Mode list copied.", 13, 10, 0
msg_novbe:     db "No usable VESA BIOS found.", 13, 10, 0
msg_fallback:  db "No listed 32bpp mode; trying 1024x768x32.", 13, 10, 0
msg_mode:      db "Selected mode ", 0
msg_dbgmode:   db "  mode ", 0
msg_space:     db " ", 0
msg_x:         db "x", 0
msg_bpp:       db " 32bpp", 13, 10, 0
msg_load:      db "Loading kernel: ", 0
msg_nl:        db 13, 10, 0
msg_loading:   db "Starting ReverticalOS at ", 0
msg_pm:        db "Entering protected mode.", 13, 10, 0
msg_diskerr:   db "Disk read error while loading the kernel.", 13, 10, 0
msg_badhdr:    db "Invalid kernel header.", 13, 10, 0
msg_vidfail:   db "Unable to set a VESA video mode.", 13, 10, 0
msg_halt:      db "Boot halted. Press Ctrl+Alt+Del to restart.", 13, 10, 0

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF        ; 0x08: 32-bit code, base 0, limit 4 GiB
    dq 0x00CF92000000FFFF        ; 0x10: data
    dq 0x00AF9A000000FFFF        ; 0x18: 64-bit code
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

; Long-mode GDT installed immediately before entering the kernel.
align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF        ; 0x08: 64-bit code
    dq 0x00CF92000000FFFF        ; 0x10: data
gdt64_end:
gdt64_desc:
    dw gdt64_end - gdt64 - 1
    dq gdt64

; live variables
best_score:    dd 0
best_mode:     dw 0xFFFF
best_w:        dw 0
best_h:        dw 0
best_pitch:    dw 0
best_phys:     dd 0
cur_mode:      dw 0
mode_ptr:      dw MODE_LIST
modes_seen:    dw 0
kernel_size:   dd 0
kernel_dest:   dd KERNEL_DEST
sectors_left:  dd 0
chunk_sectors: dw 0
retries:       db 0

%ifdef CDBOOT
boot_sector_shift: db 11
boot_seg_advance:  db 7
boot_chunk_max:    dw 8
boot_kernel_lba:   dd KERNEL_LBA
boot_kernel_data:  dd KERNEL_DATA_LBA
%endif

align 4
dap_load:
    db 0x10, 0
dap_count:     dw 0
dap_off:       dw 0
dap_seg:       dw 0
dap_lba:       dq 0

; ============================================================================
;  32-bit protected mode
; ============================================================================
BITS 32
pm_entry:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; ---------------------------------------------------- identity tables ---
    ; Page tables live at 0x6000, safely below stage 2 itself (0x7E00).
    ; 0x6000 PML4, 0x6100 PDPT, 0x6200..0x6500 four page directories.
    ; 2048 entries of 2 MiB map the first 4 GiB, which covers the kernel,
    ; all low MMIO and the VBE framebuffer.
    mov     dword [0x6000], 0x00006103
    mov     dword [0x6004], 0
    mov     dword [0x6100], 0x00006203
    mov     dword [0x6104], 0
    mov     dword [0x6108], 0x00006303
    mov     dword [0x610C], 0
    mov     dword [0x6110], 0x00006403
    mov     dword [0x6114], 0
    mov     dword [0x6118], 0x00006503
    mov     dword [0x611C], 0

    mov     edi, 0x6200
    mov     eax, 0x00000083            ; present | rw | 2 MiB page
    mov     ecx, 2048
.fill_pd:
    mov     [edi], eax
    mov     dword [edi + 4], 0
    add     edi, 8
    add     eax, 0x200000
    loop    .fill_pd

    ; ------------------------------------------------------- long mode -----
    mov     eax, cr4
    or      eax, 0x20                  ; PAE
    mov     cr4, eax

    mov     ecx, 0xC0000080            ; EFER
    rdmsr
    or      eax, 0x100                 ; LME
    wrmsr

    mov     eax, 0x6000
    mov     cr3, eax

    mov     eax, cr0
    or      eax, 0x80000001            ; PG | PE
    mov     cr0, eax

    jmp     0x18:long_entry

BITS 64
long_entry:
    ; The protected-mode GDT keeps 0x08 as a 32-bit code segment.  The kernel
    ; and its interrupt gates expect 0x08 to be the 64-bit code selector, so
    ; load a long-mode GDT before the handoff.
    lgdt    [rel gdt64_desc]
    push    qword 0x08
    lea     rax, [rel long_ready]
    push    rax
    retfq
long_ready:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     rsp, 0x90000
    xor     rbp, rbp
    mov     rdi, 0x7000
    jmp     0x100000

times 16*512-8-($-$$) db 0
; Signature checked by boot/cdstage1.asm after loading this image from the ISO.
db 'RVSTG2CD'
