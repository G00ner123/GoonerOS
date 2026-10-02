[bits 32]

global gdt_flush
global user_enter
global user_kernel_stack
global user_faulted
extern user_syscall_dispatch
extern user_fault_dispatch
extern paging_activate_kernel_space

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    push dword 0x08
    push dword .reload
    retf
.reload:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

user_enter:
    push ebp
    push ebx
    push esi
    push edi
    mov [user_kernel_stack], esp
    pushfd
    pop dword [user_kernel_eflags]
    mov eax, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    push dword 0x23
    push ecx
    push dword 0x202
    push dword 0x1B
    push eax
    mov eax, edx
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    iret

user_syscall:
    pusha
    cld
    push esp
    call user_syscall_dispatch
    add esp, 4
    test eax, eax
    jnz user_return_to_kernel
    popa
    iretd

user_page_fault:
    pusha
    cld
    mov eax, cr2
    mov ebx, [esp + 40]
    mov ecx, [esp + 36]
    mov edx, [esp + 32]
    push eax
    push ebx
    push ecx
    push edx
    call user_fault_dispatch
    add esp, 16
    jmp user_return_to_kernel

user_general_protection:
    pusha
    cld
    xor eax, eax
    mov ebx, [esp + 44]
    mov ecx, [esp + 40]
    mov edx, [esp + 36]
    push eax
    push ebx
    push ecx
    push edx
    call user_fault_dispatch
    add esp, 16
    jmp user_return_to_kernel

user_exception_return:
user_return_to_kernel:
    mov esp, [user_kernel_stack]
    call paging_activate_kernel_space
    push dword [user_kernel_eflags]
    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    mov eax, 1
    ret

global isr_syscall
global isr_user_page_fault
global isr_user_general_protection
global user_exception_return

isr_syscall:
    cli
    jmp user_syscall

isr_user_page_fault:
    cli
    jmp user_page_fault

isr_user_general_protection:
    cli
    push dword 13
    jmp user_general_protection

section .bss
align 4
user_kernel_stack: resd 1
user_kernel_eflags: resd 1
user_faulted: resd 1
