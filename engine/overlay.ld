/* Code loaded per level (build.sh build_overlays: a monster's code, tables and data, linked
   against game.elf's symbols): one section from OVL_BASE, its head (the spawn function's
   address) first, its zeroed data in the image too. tools/overlay.py finds what needs moving
   by linking it at two bases. */
OUTPUT_FORMAT("elf32-sh")
ENTRY(_ovl_spawn)

SECTIONS
{
    . = OVL_BASE;
    .ovl : ALIGN(16)
    {
        KEEP(*(.ovlhead))
        *(.text .text.* .text.unlikely .text.unlikely.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
    }
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
