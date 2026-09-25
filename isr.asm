[bits 32]

global _start
extern kernel_main
extern _bss_start
extern _bss_end

section .text.boot

_start:
    cli
    mov esp, stack_top

    ; ebx/ecx/edx (Framebuffer Adresse/Pitch/BPP) kommen vom Bootloader und
    ; werden von kernel_main ausgelesen. WICHTIG: der Stack liegt selbst im
    ; .bss-Bereich (stack_top ist Teil davon), darum NICHT auf den Stack
    ; pushen - die BSS-Nullung unten würde die gepushten Werte sonst mit
    ; überschreiben! Nur ecx zwischenparken (wird fuer die Zaehlschleife
    ; gebraucht), ebx/edx fasst rep stosb gar nicht an.
    mov esi, ecx

    ; .bss nullen, bevor irgendein C-Code laeuft - globale Variablen ohne
    ; Startwert MUESSEN hier wirklich 0 sein, sonst gibt's Datenmuell-Bugs
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
