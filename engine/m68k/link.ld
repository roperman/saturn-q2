/* The 68000 sees sound RAM from address 0: vectors, then the driver. It must
   stay under 0x7000 or so, below its stack and the sound bank at 0x8000. */
ENTRY(_start)
SECTIONS
{
    .vectors 0x000 : { KEEP(*(.vectors)) }
    .text   0x400 : { *(.text*) *(.rodata*) }
    .data         : { *(.data*) }
    .bss          : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(2); __bss_end = .; }
    /DISCARD/     : { *(.comment) *(.note*) }
}
