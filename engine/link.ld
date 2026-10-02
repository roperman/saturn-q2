/* Bare-metal layout: program at 0x06004000 in high work RAM, stack at the top.
   Code that runs only at start-up, a level's start, in the menus or seldom
   (.lwtext: these objects' code, and every function marked cold) lives in low
   work RAM instead, leaving high work RAM to the levels: it's loaded after
   .data, crt0 copies it down, and .bss is laid over where it was loaded. */
OUTPUT_FORMAT("elf32-sh")
ENTRY(_start)

MEMORY
{
    HWRAM (rwx) : ORIGIN = 0x06004000, LENGTH = 0x000F4000   /* to 0x060F8000; slave stack 0x060F8000-0x060FC000, master stack 0x060FC000-0x06100000 */
    LWRAM (rwx) : ORIGIN = 0x00200000, LENGTH = 0x00100000   /* (the levels' data after .lwtext: src/level.c) */
}

SECTIONS
{
    .text : ALIGN(4)
    {
        *(.start)
        *(EXCLUDE_FILE(*level.o *menu.o *cd.o *g_target.o *m_gunner.o *m_berserk.o *cycles.o) .text)
        *(.text.hot .text.hot.* .text.startup .text.startup.*)
        *(.rodata .rodata.*)
    } > HWRAM

    .data : ALIGN(4)
    {
        *(.data .data.*)
    } > HWRAM

    .lwtext : ALIGN(16)
    {
        __lwtext_start = .;
        *level.o(.text) *menu.o(.text) *cd.o(.text) *g_target.o(.text) *m_gunner.o(.text) *m_berserk.o(.text)
        *cycles.o(.text)
        *(.text.unlikely .text.unlikely.*)
        . = ALIGN(16);
        __lwtext_end = .;
    } > LWRAM AT > HWRAM
    __lwtext_load = LOADADDR(.lwtext);

    .bss ALIGN(ADDR(.data) + SIZEOF(.data), 16) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > HWRAM

    /DISCARD/ : { *(.comment) *(.eh_frame) }
}
