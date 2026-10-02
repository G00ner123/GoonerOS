ENTRY(kernel_main)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x100000,
    LENGTH = 512K
}

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x100000;
    _kernel_text_start = .;

    .text :
    {
        *(.text.boot)
        *(.text)
        . = ALIGN(4096);
    } > RAM :text
    ASSERT(. <= 0x13F800, "Code zu gross!")

    .rodata ALIGN(4096) :
    {
        _kernel_ro_start = .;
        *(.rodata*)
        *(.eh_frame*)
        *(.gcc_except_table*)
        . = ALIGN(16);
        _user_app_start = .;
        KEEP(*(.userapp))
        _user_app_end = .;
        . = ALIGN(4096);
        _kernel_ro_end = .;
    } > RAM :rodata

    .data ALIGN(4096) :
    {
        *(.data)
    } > RAM :data
    ASSERT(. <= 0x13F800, "Kerneldatei zu gross!")

    .bss ALIGN(4096) :
    {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        _bss_end = .;
    } > RAM :data
    ASSERT(. <= 0x17F800, "Kernel-RAM-Bereich zu gross!")
}
