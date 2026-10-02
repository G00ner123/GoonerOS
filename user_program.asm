bits 32
section .text
global _start

_start:
    mov eax, 1
    mov ebx, message
    mov ecx, message_end - message
    int 0x80
    mov eax, 2
    mov ebx, 42
    int 0x80
.exit:
    jmp .exit

message: db "ELF32 program loaded from disk", 10
message_end:
