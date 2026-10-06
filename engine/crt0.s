! Entry point: the BIOS loads 0.BIN to 0x06004000 (IP.BIN's first-read
! address) and jumps here. Mask interrupts, set the stack, purge + enable the
! SH-2 cache, copy the code that lives in low work RAM there (link.ld's
! .lwtext: loaded after .data, where .bss goes), clear BSS, call main.

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

        mov.l   lw_load, r1
        mov.l   lw_start, r2
        mov.l   lw_end, r3
4:      cmp/hs  r3, r2
        bt      5f
        mov.l   @r1+, r0
        mov.l   r0, @r2
        bra     4b
        add     #4, r2
5:
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
lw_load:    .long __lwtext_load
lw_start:   .long __lwtext_start
lw_end:     .long __lwtext_end
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

! void chip_call2(void *fn, u32 a, u32 b): fn(a, b) run on this CPU's on-chip
! stack, the 2 KB of its cache that two-way mode frees (OPT=-DCACHE_STACK:
! engine/sat.h chip_init). The work RAM stack is kept in r8 meanwhile, and at
! 0xC00007F8 for hw_call2. Already on the on-chip stack: a plain call.
        .global _chip_call2
        .align 2
_chip_call2:
        mov.l   chip_base, r1
        cmp/hs  r1, r15         ! T: r15 >= 0xC0000000, on the on-chip stack
        bt      plain_call
        mov.l   r8, @-r15
        sts.l   pr, @-r15
        mov     r15, r8
        mov.l   chip_hwsp, r1
        mov.l   r8, @r1
        mov.l   chip_top, r15
        mov     r4, r0
        mov     r5, r4
        jsr     @r0
        mov     r6, r5
        mov     r8, r15
        lds.l   @r15+, pr
        rts
        mov.l   @r15+, r8
plain_call:
        mov     r4, r0
        mov     r5, r4
        jmp     @r0
        mov     r6, r5
! void hw_call2(void *fn, u32 a, u32 b): fn(a, b) back on the work RAM stack
! (for the few functions with frames too big for the on-chip stack: the gun,
! the game's step, dl_face). Not on the on-chip stack: a plain call.
        .global _hw_call2
        .align 2
_hw_call2:
        mov.l   chip_base, r1
        cmp/hs  r1, r15
        bf      plain_call
        mov.l   r8, @-r15
        sts.l   pr, @-r15
        mov     r15, r8
        mov.l   chip_hwsp, r1
        mov.l   @r1, r15
        mov     r4, r0
        mov     r5, r4
        jsr     @r0
        mov     r6, r5
        mov     r8, r15
        lds.l   @r15+, pr
        rts
        mov.l   @r15+, r8
        .align 2
chip_base:   .long 0xC0000000
chip_top:    .long 0xC00007F0
chip_hwsp:   .long 0xC00007F8
