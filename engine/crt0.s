! Entry point: the BIOS loads 0.BIN to 0x06004000 (IP.BIN's first-read
! address) and jumps here. Mask interrupts, set the stack, purge + enable the
! SH-2 cache, clear BSS, call main.

        .section .start, "ax"
        .global _start
_start:
        mov.l   sr_mask, r0
        ldc     r0, sr
        mov.l   stack_top, r15

        mov.l   ccr, r1
        mov     #0x10, r0       ! CP: purge
        mov.b   r0, @r1
        mov     #0x01, r0       ! CE: enable, 4-way
        mov.b   r0, @r1

        mov.l   bss_start, r1
        mov.l   bss_end, r2
        mov     #0, r0
1:      cmp/hs  r2, r1
        bt      2f
        mov.l   r0, @r1
        bra     1b
        add     #4, r1
2:
        mov.l   main_addr, r0
        jsr     @r0
        nop
3:      bra     3b
        nop

        .align 2
sr_mask:    .long 0x000000F0
stack_top:  .long 0x06100000    ! top of high work RAM
ccr:        .long 0xFFFFFE92
bss_start:  .long __bss_start
bss_end:    .long __bss_end
main_addr:  .long _main

! Slave SH-2 entry (installed as its vector 0x94 via the BIOS SYS_SETSINT
! call, then SMPC SSHON). Own stack below the master's, cache on, interrupts
! masked, then into C.
        .text
        .global _slave_entry
        .align 2
_slave_entry:
        mov.l   s_sr_mask, r0
        ldc     r0, sr
        mov.l   s_stack_top, r15
        mov.l   s_ccr, r1
        mov     #0x10, r0
        mov.b   r0, @r1
        mov     #0x01, r0
        mov.b   r0, @r1
        mov.l   s_main, r0
        jmp     @r0
        nop
        .align 2
s_sr_mask:   .long 0x000000F0
s_stack_top: .long 0x060FC000
s_ccr:       .long 0xFFFFFE92
s_main:      .long _slave_main
