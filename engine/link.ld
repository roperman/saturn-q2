/* Bare-metal layout: program at 0x06004000 in high work RAM, stack at the top. */
OUTPUT_FORMAT("elf32-sh")
ENTRY(_start)

MEMORY
{
    HWRAM (rwx) : ORIGIN = 0x06004000, LENGTH = 0x000F4000   /* to 0x060F8000; slave stack 0x060F8000-0x060FC000, master stack 0x060FC000-0x06100000 */
}

SECTIONS
{
    .text : ALIGN(4)
    {
        *(.start)
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > HWRAM

    .data : ALIGN(4)
    {
        *(.data .data.*)
    } > HWRAM

    .bss (NOLOAD) : ALIGN(16)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > HWRAM

    /DISCARD/ : { *(.comment) *(.eh_frame) }
}
