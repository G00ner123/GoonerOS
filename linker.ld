ENTRY(kernel_main)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x100000,
    LENGTH = 256K
}

SECTIONS
{
    . = 0x100000;

    .text :
    {
        *(.text.boot)
        *(.text)
    } > RAM
    ASSERT(. <= 0x13F800, "Code zu gross!")

    .rodata :
    {
        *(.rodata*)
    } > RAM

    .data :
    {
        *(.data)
    } > RAM

    .bss :
    {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        _bss_end = .;
    } > RAM
    ASSERT(. <= 0x13F800, "Kernel zu gross!")
}
