| The 68000 sound driver's start-up. The SH-2 copies the driver to the bottom
| of sound RAM and releases the 68000 (SMPC SNDON); it reads its stack and
| first PC from the vector table at address 0.
        .section .vectors,"a"
        .long   0x00002000              | stack: grows down from the sound bank (SND_BANK_BASE)
        .long   _start
        .rept   254
        .long   unhandled               | the driver polls; no interrupts used
        .endr

        .text
        .globl  _start
_start:
        move.w  #0x2700,%sr             | supervisor, interrupts masked
        lea     __bss_start,%a0         | clear .bss
        lea     __bss_end,%a1
1:      cmp.l   %a1,%a0
        bcc.s   2f
        clr.w   (%a0)+
        bra.s   1b
2:      jsr     main
3:      bra.s   3b

unhandled:
        rte

        .section .note.GNU-stack,"",%progbits
