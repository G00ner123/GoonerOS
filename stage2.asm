org 0x7E00
bits 16

start_stage2:
    mov [boot_drive], dl
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov si, msg_boot
    call print_string

    mov ax, 0x4F01
    mov cx, 0x4118
    mov di, vbe_mode_info
    int 0x10
    cmp ax, 0x004F
    jne .vbe_getinfo_fail

    mov si, msg_getinfo_ok
    call print_string

    ; BIOS-DAPs duerfen hier hoechstens 127 Sektoren laden. Vier
    ; zusammenhaengende Transfers laden deshalb 508 Sektoren (254 KiB)
    ; an physisch zusammenhaengende Adressen ab 0x10000.
    mov word [dap_kernel + 2], 127
    mov word [dap_kernel + 4], 0
    mov word [dap_kernel + 6], 0x1000
    mov dword [dap_kernel + 8], 3
    mov cx, 4
.load_kernel:
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap_kernel
    int 0x13
    jc .disk_fail
    add word [dap_kernel + 6], 0x0FE0
    add dword [dap_kernel + 8], 127
    dec cx
    jnz .load_kernel

    mov si, msg_disk_ok
    call print_string

    mov ax, 0x4F02
    mov bx, 0x4118
    int 0x10
    cmp ax, 0x004F
    jne .vbe_setmode_fail

    mov si, msg_setmode_ok
    call print_string

    in al, 0x92
    or al, 2
    out 0x92, al

    jmp .continue_boot

.vbe_getinfo_fail:
    mov si, msg_vbe_getinfo_fail
    call print_string
    jmp hang

.vbe_setmode_fail:
    mov si, msg_vbe_setmode_fail
    call print_string
    jmp hang

.disk_fail:
    mov si, msg_disk_fail
    call print_string
    jmp hang

.continue_boot:
    cli
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000

    ; Jetzt im Protected Mode flach nach 0x100000 kopieren, wo
    ; Linker/kernel_main ihn erwarten.
    mov esi, 0x10000
    mov edi, 0x100000
    mov ecx, (508*512)/4   ; vier BIOS-Transfers mit je 127 Sektoren
    rep movsd

    ; Framebuffer-Infos erst jetzt lesen (ecx wurde oben für rep movsd gebraucht)
    mov ebx, [vbe_mode_info + 0x28]        ; physische Framebuffer-Adresse
    movzx ecx, word [vbe_mode_info + 0x10] ; Bytes pro Bildzeile (Pitch)
    movzx edx, byte [vbe_mode_info + 0x19] ; Bits pro Pixel

    mov eax, 0x100000
    jmp eax

hang:
    hlt
    jmp hang

print_string:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print_string
.done:
    ret

msg_boot: db "Stage2 ", 0
msg_getinfo_ok: db "GetInfoOK ", 0
msg_setmode_ok: db "SetModeOK ", 0
msg_disk_ok: db "DiskOK ", 0
msg_vbe_getinfo_fail: db "VBE-GETINFO-FAIL", 0
msg_vbe_setmode_fail: db "VBE-SETMODE-FAIL", 0
msg_disk_fail: db "DISK-READ-FAIL", 0

boot_drive: db 0
vbe_mode_info: times 256 db 0

dap_kernel:
    db 0x10
    db 0
    dw 127
    dw 0x0000, 0x1000   ; wird vor dem Laden fuer jeden Transfer gesetzt
    dq 3

gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0
    db 0
    db 10011010b
    db 11001111b
    db 0
gdt_data:
    dw 0xFFFF, 0
    db 0
    db 10010010b
    db 11001111b
    db 0
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; Auf exakt 2 Sektoren (1024 Bytes) auffuellen - boot.asm liest fuer stage2
; immer genau 2 Sektoren, unabhaengig davon wie gross der Code hier gerade ist
times 1024-($-$$) db 0
