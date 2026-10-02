bits 32
section .text
align 4

extern irq0_handler
extern irq1_handler
extern irq12_handler
extern kernel_exception_dispatch
extern user_exception_return
extern user_interrupt_return
extern scheduler_timer_switch

global irq0
global irq1
global irq12
global exception_stub_table

%macro EXCEPTION_NO_ERROR 1
exception_%1:
    cli
    push dword 0
    push dword %1
    jmp near exception_common_stub
%endmacro

%macro EXCEPTION_ERROR 1
exception_%1:
    cli
    push dword %1
    jmp near exception_common_stub
%endmacro

EXCEPTION_NO_ERROR 0
EXCEPTION_NO_ERROR 1
EXCEPTION_NO_ERROR 2
EXCEPTION_NO_ERROR 3
EXCEPTION_NO_ERROR 4
EXCEPTION_NO_ERROR 5
EXCEPTION_NO_ERROR 6
EXCEPTION_NO_ERROR 7
EXCEPTION_ERROR 8
EXCEPTION_NO_ERROR 9
EXCEPTION_ERROR 10
EXCEPTION_ERROR 11
EXCEPTION_ERROR 12
EXCEPTION_ERROR 13
EXCEPTION_ERROR 14
EXCEPTION_NO_ERROR 15
EXCEPTION_NO_ERROR 16
EXCEPTION_ERROR 17
EXCEPTION_NO_ERROR 18
EXCEPTION_NO_ERROR 19
EXCEPTION_NO_ERROR 20
EXCEPTION_ERROR 21
EXCEPTION_NO_ERROR 22
EXCEPTION_NO_ERROR 23
EXCEPTION_NO_ERROR 24
EXCEPTION_NO_ERROR 25
EXCEPTION_NO_ERROR 26
EXCEPTION_NO_ERROR 27
EXCEPTION_NO_ERROR 28
EXCEPTION_ERROR 29
EXCEPTION_ERROR 30
EXCEPTION_NO_ERROR 31

section .rodata
exception_stub_table:
%assign i 0
%rep 32
    dd exception_%+i
%assign i i+1
%endrep

section .text
exception_common_stub:
    pusha
    cld
    mov eax, [esp + 32]
    mov ebx, [esp + 36]
    mov ecx, [esp + 40]
    mov edx, [esp + 44]
    mov esi, cr2
    push esi
    push edx
    push ecx
    push ebx
    push eax
    call kernel_exception_dispatch
    add esp, 20
    cmp eax, 2
    je .schedule_user
    test eax, eax
    jnz user_exception_return
    popa
    add esp, 8
    iretd
.schedule_user:
    mov eax, esp
    push eax
    call scheduler_timer_switch
    add esp, 4
    mov esp, eax
    jmp user_interrupt_return

irq0:
    cli
    push dword 0
    push dword 32
    jmp irq_common_stub

irq1:
    cli
    push dword 0
    push dword 33
    jmp irq_common_stub

irq12:
    cli
    push dword 0
    push dword 44
    jmp irq_common_stub

irq_common_stub:
    pusha

    cmp dword [esp + 32], 32
    je .timer

    cmp dword [esp + 32], 33
    je .keyboard

    cmp dword [esp + 32], 44
    je .mouse

    jmp .done

.timer:
    mov eax, esp
    push eax
    call irq0_handler
    add esp, 4
    mov esp, eax
    jmp .done

.keyboard:
    call irq1_handler
    jmp .done

.mouse:
    call irq12_handler

.done:
    popa
    add esp, 8
    sti
    iret
