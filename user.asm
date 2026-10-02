[bits 32]

section .userapp progbits alloc exec nowrite align=16
global user_app_start
global user_app_end

user_app_start:
    mov edx, eax
    mov eax, 3
    int 0x80
    add al, '0'
    mov [0x40001FF0], al
    mov eax, 1
    mov ebx, user_message - user_app_start + 0x40000000
    mov ecx, user_message_end - user_message
    int 0x80
    mov eax, 1
    mov ebx, 0x40001FF0
    mov ecx, 1
    int 0x80
    mov eax, 1
    mov ebx, user_suffix - user_app_start + 0x40000000
    mov ecx, user_suffix_end - user_suffix
    int 0x80
    test edx, edx
    jz .exit
    mov eax, [0x00100000]
.exit:
    mov eax, 2
    int 0x80
    jmp .exit

user_message: db "ring-3 process ", 0
user_message_end:
user_suffix: db " completed", 10
user_suffix_end:
user_app_end:
