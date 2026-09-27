! The 68000 sound driver (engine/m68k/, built by build.inc.sh), carried in the
! SH-2 program; engine/snd.c copies it into sound RAM.
        .section .rodata
        .global _snd68k_bin
        .global _snd68k_bin_end
        .align 4
_snd68k_bin:
        .incbin "obj/gen/snd68k.bin"
_snd68k_bin_end:
        .align 4
