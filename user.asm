[bits 32]

section .userapp progbits alloc exec nowrite align=16
global user_app_start
global user_app_end

user_app_start:
    mov ebp, eax
    cmp ebp, 2
    je .spin
    test ebp, ebp
    jnz .start
    mov ecx, 10000000
.work:
    dec ecx
    jnz .work
.start:
    mov eax, 3
    int 0x80
    mov ebx, 10
    mov ecx, 0
    mov esi, 0x40002FF0
.decimal:
    xor edx, edx
    div ebx
    add dl, '0'
    mov [esi+ecx], dl
    inc ecx
    test eax, eax
    jnz .decimal
    mov edx, ecx
    dec ecx
    mov edi, 0
.reverse:
    cmp edi, ecx
    jge .pid_ready
    mov al, [esi+edi]
    mov ah, [esi+ecx]
    mov [esi+edi], ah
    mov [esi+ecx], al
    inc edi
    dec ecx
    jmp .reverse
.pid_ready:
    mov ecx, edx
    mov eax, 1
    mov ebx, user_message - user_app_start + 0x40000000
    mov ecx, user_message_end - user_message
    int 0x80
    mov eax, 1
    mov ebx, esi
    mov ecx, edx
    int 0x80
    cmp ebp, 5
    je .guard_fault
    cmp ebp, 1
    je .fault
    cmp ebp, 3
    jne .no_sleep
    mov eax, 5
    mov ebx, 10000
    int 0x80
.no_sleep:
    cmp ebp, 4
    jne .complete
    mov eax, 6
    mov ebx, 100
    int 0x80
.complete:
    jmp .print_suffix
.fault:
    mov eax, [0x00100000]
.guard_fault:
    mov eax, [0x40001000]
.print_suffix:
    mov eax, 1
    mov ebx, user_suffix - user_app_start + 0x40000000
    mov ecx, user_suffix_end - user_suffix
    int 0x80
.exit:
    mov eax, 2
    xor ebx, ebx
    int 0x80
    jmp .exit
.spin:
    mov eax, 4
    int 0x80
    inc ecx
    jmp .spin

user_message: db "ring-3 process ", 0
user_message_end:
user_suffix: db " completed", 10
user_suffix_end:
user_app_end:
