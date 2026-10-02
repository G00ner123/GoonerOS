[bits 32]

global _start
global stack_top
extern kernel_main
extern _bss_start
extern _bss_end

section .text.boot

_start:
    cli
    mov esp, stack_top

    ;
    ;
    ;
    ;
    ;
    mov esi, ecx

    ; 
    ; Hier is 0
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    mov ecx, esi
    call kernel_main
    hlt

section .bss
align 16

stack_bottom:
    resb 16384

stack_top:
