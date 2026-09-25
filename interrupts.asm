bits 32
section .text
align 4

extern irq0_handler
extern irq1_handler
extern irq12_handler

global isr0
global isr1
global irq0
global irq1
global irq12

isr0:
    cli
    push dword 0
    push dword 0
    jmp isr_common_stub

isr1:
    cli
    push dword 0
    push dword 1
    jmp isr_common_stub

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

isr_common_stub:
    pusha
    call isr_handler_stub
    popa
    add esp, 8
    iret

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
    call irq0_handler
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

isr_handler_stub:
    ret
