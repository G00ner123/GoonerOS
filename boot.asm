section .text
org 0x7C00
bits 16

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap_stage2
    int 0x13
    jc hang

    mov dl, [boot_drive]
    jmp 0x0000:0x7E00

hang:
    hlt
    jmp hang

boot_drive: db 0

dap_stage2:
    db 0x10
    db 0
    dw 2
    dw 0x7E00, 0x0000   ; offset:segment -> physisch 0x7E00
    dq 1

times 510-($-$$) db 0
dw 0xAA55
