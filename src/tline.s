! A line through the map in SH-2 assembly (src/trace.c's line_check, which it
! stands in for): down the BSP along the line; the first leaf, in order along
! the line, whose contents match the mask is the hit.
!
!   int line_asm(line_args *a, int num)
!
! 1: clear all the way; 0: a hit (a->p1 where, a->p1f how far, a->entered the
! plane crossed into its leaf, a->contents the leaf's); 2: the kept parts ran
! out of room (the C does it instead).
!
! Down a side the line's wholly on, a jump. Where a plane cuts it, the far part
! is kept (in a's stack: 40 bytes, num p1f p2f p1[3] p2[3] entered) and the near
! part walked; a clear leaf takes up the last part kept. The C made a call a
! node (six arguments: two on the stack) and read lv's bases each time. The
! crossing's fraction by the divider and the midpoint by the C's own
! truncating multiplies (a product at a time): the same results, bit for bit.

        .text
        .align  2
        .global _line_asm

L_NODES  = 0
L_PLANES = 4
L_LEAFS  = 8
L_MASK   = 12
L_P1     = 16                           ! x y z (16.16)
L_P2     = 28
L_P1F    = 40
L_P2F    = 44
L_ENT    = 48                           ! the plane crossed into this part (0: none yet)
L_CONT   = 52                           ! out: the hit leaf's contents
L_STACK  = 56                           ! the kept far parts
L_DEPTH  = 64                           ! (as many; LINE_STACK in src/trace.c)
EPS      = 2048                         ! DIST_EPSILON: 1/32

! fmul(\a, \b) into \out (the C's: (s64)a * b >> 16). Uses \t
.macro  FMUL a, b, out, t
        dmuls.l \a,\b
        sts     mach,\t
        sts     macl,\out
        xtrct   \t,\out
.endm

_line_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        mov     r4,r14
        mov     r5,r10                  ! num
        mov.l   @(L_NODES,r14),r13
        mov.l   @(L_PLANES,r14),r12
        mov     r14,r11
        add     #L_STACK,r11            ! nothing kept
        bra     .Lwalk
        nop

.Lleaf:
        ! leafs + (-1 - num) * 28
        not     r10,r0
        mov     r0,r1
        shll2   r1
        shll2   r1
        shll    r1                      ! 32
        shll2   r0                      ! 4
        sub     r0,r1                   ! 28
        mov.l   @(L_LEAFS,r14),r2
        add     r1,r2
        mov.l   @(24,r2),r1             ! its contents
        mov.l   @(L_MASK,r14),r0
        tst     r0,r1
        bf      .Lhit
        ! clear: on with the last part kept, if any
        mov     r14,r0
        add     #L_STACK,r0
        cmp/eq  r0,r11
        bt      .Lclear
        add     #-40,r11
        mov.l   @r11,r10
        mov.l   @(4,r11),r0
        mov.l   r0,@(L_P1F,r14)
        mov.l   @(8,r11),r0
        mov.l   r0,@(L_P2F,r14)
        mov.l   @(12,r11),r0
        mov.l   r0,@(L_P1,r14)
        mov.l   @(16,r11),r0
        mov.l   r0,@(L_P1+4,r14)
        mov.l   @(20,r11),r0
        mov.l   r0,@(L_P1+8,r14)
        mov.l   @(24,r11),r0
        mov.l   r0,@(L_P2,r14)
        mov.l   @(28,r11),r0
        mov.l   r0,@(L_P2+4,r14)
        mov.l   @(32,r11),r0
        mov.l   r0,@(L_P2+8,r14)
        mov.l   @(36,r11),r0
        bra     .Lwalk
        mov.l   r0,@(L_ENT,r14)

.Lhit:
        mov.l   r1,@(L_CONT,r14)
        bra     .Lout
        mov     #0,r0
.Lclear:
        mov     #1,r0
.Lout:
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

.Lwalk:
        cmp/pz  r10
        bf      .Lleaf
        ! the node: nodes + num * 24; its plane: planes + index * 20
        mov     r10,r9
        shll    r9
        add     r10,r9                  ! 3 num
        shll2   r9
        shll    r9                      ! 24 num
        add     r13,r9                  ! the node
        mov.w   @r9,r7
        extu.w  r7,r7                   ! its plane's index
        mov     r7,r2
        shll2   r2
        add     r7,r2
        shll2   r2
        add     r12,r2                  ! the plane
        mov     #16,r0
        mov.b   @(r0,r2),r3
        extu.b  r3,r3                   ! its type
        mov.l   @(12,r2),r4             ! its distance
        mov     #3,r1
        cmp/hs  r1,r3
        bt      .Lslant
        ! on an axis: t1 = p1[type] - dist, t2 = p2[type] - dist
        shll2   r3
        mov     r3,r0
        add     #L_P1,r0
        mov.l   @(r0,r14),r5
        add     #L_P2-L_P1,r0
        mov.l   @(r0,r14),r6
        sub     r4,r5
        bra     .Lsides
        sub     r4,r6

.Lslant:
        ! t = the plane's normal . p - dist, each product truncated as the C's
        mov.l   @(L_P1,r14),r0
        mov.l   @r2,r1
        FMUL    r0,r1,r5,r3
        mov.l   @(L_P1+4,r14),r0
        mov.l   @(4,r2),r1
        FMUL    r0,r1,r6,r3
        add     r6,r5
        mov.l   @(L_P1+8,r14),r0
        mov.l   @(8,r2),r1
        FMUL    r0,r1,r6,r3
        add     r6,r5
        sub     r4,r5                   ! t1
        mov.l   @(L_P2,r14),r0
        mov.l   @r2,r1
        FMUL    r0,r1,r6,r3
        mov.l   @(L_P2+4,r14),r0
        mov.l   @(4,r2),r1
        FMUL    r0,r1,r8,r3
        add     r8,r6
        mov.l   @(L_P2+8,r14),r0
        mov.l   @(8,r2),r1
        FMUL    r0,r1,r8,r3
        add     r8,r6
        sub     r4,r6                   ! t2

.Lsides:
        ! r5 t1, r6 t2, r9 the node, r7 its plane's index
        cmp/pz  r5
        bf      .Lt1neg
        cmp/pz  r6
        bf      .Lcross                 ! in front, then behind
        mov.w   @(2,r9),r0              ! in front all the way: child 0
        bra     .Lwalk
        mov     r0,r10
.Lt1neg:
        cmp/pz  r6
        bt      .Lcross                 ! behind, then in front
        mov.w   @(4,r9),r0              ! behind all the way: child 1
        bra     .Lwalk
        mov     r0,r10

.Lcross:
        ! is there room to keep the far part?
        mov     r14,r0
        add     #L_STACK,r0
        mov.l   .Lroom,r1
        add     r1,r0
        cmp/hs  r0,r11
        bf      1f
        bra     .Lout
        mov     #2,r0
1:      ! the fraction where it crosses, a hair short of the plane: n / d, 0 to 1
        ! (the C's frac_div, clamped): n = t1 -+ EPS, d = t1 - t2, both turned
        ! positive when t1's behind; r8 the side it starts on
        mov     r5,r1
        sub     r6,r1                   ! d
        mov.w   .Leps,r3
        cmp/pz  r5
        bt      2f
        add     r3,r5
        neg     r5,r5
        neg     r1,r1
        bra     3f
        mov     #1,r8
2:      sub     r3,r5
        mov     #0,r8
3:      cmp/pl  r5
        bf      .Lf0                    ! n <= 0: 0
        cmp/ge  r1,r5
        bt      .Lf1                    ! n >= d: 1
        mov.l   .Ldivu,r3               ! (n << 16) / d, 64 by 32
        mov     r5,r0
        shlr16  r0
        mov.l   r1,@r3                  ! DVSR
        mov.l   r0,@(16,r3)             ! DVDNTH
        shll16  r5
        mov.l   r5,@(20,r3)             ! DVDNTL: it starts
        bra     .Lfrac
        mov.l   @(20,r3),r5             ! (waits for it)
.Lf0:
        bra     .Lfrac
        mov     #0,r5
.Lf1:
        mov.l   .Lone,r5
.Lfrac:
        ! the far part kept: from the crossing (midf, mid) to p2 (p2f), entered
        ! across the plane turned the way it's crossed; the near part walked on:
        ! p1 to the crossing
        mov.l   @(L_P2F,r14),r2
        mov.l   @(L_P1F,r14),r4
        mov     r2,r0
        sub     r4,r0
        FMUL    r0,r5,r6,r3
        add     r4,r6                   ! midf
        mov.l   r6,@(4,r11)
        mov.l   r2,@(8,r11)
        mov.l   r6,@(L_P2F,r14)
        mov.l   @(L_P2,r14),r2
        mov.l   @(L_P1,r14),r4
        mov     r2,r0
        sub     r4,r0
        FMUL    r0,r5,r6,r3
        add     r4,r6                   ! mid x
        mov.l   r6,@(12,r11)
        mov.l   r2,@(24,r11)
        mov.l   r6,@(L_P2,r14)
        mov.l   @(L_P2+4,r14),r2
        mov.l   @(L_P1+4,r14),r4
        mov     r2,r0
        sub     r4,r0
        FMUL    r0,r5,r6,r3
        add     r4,r6                   ! mid y
        mov.l   r6,@(16,r11)
        mov.l   r2,@(28,r11)
        mov.l   r6,@(L_P2+4,r14)
        mov.l   @(L_P2+8,r14),r2
        mov.l   @(L_P1+8,r14),r4
        mov     r2,r0
        sub     r4,r0
        FMUL    r0,r5,r6,r3
        add     r4,r6                   ! mid z
        mov.l   r6,@(20,r11)
        mov.l   r2,@(32,r11)
        mov.l   r6,@(L_P2+8,r14)
        ! entered: planes + (index ^ side) * 20 (a plane and its opposite are side by side)
        mov     r7,r0
        xor     r8,r0
        mov     r0,r1
        shll2   r1
        add     r0,r1
        shll2   r1
        add     r12,r1
        mov.l   r1,@(36,r11)
        ! the children: the far one's kept, the near one's next
        mov     r8,r0
        tst     r0,r0
        bf      4f
        mov.w   @(4,r9),r0              ! started in front: the far one's child 1
        mov.l   r0,@r11
        mov.w   @(2,r9),r0
        bra     5f
        mov     r0,r10
4:      mov.w   @(2,r9),r0              ! started behind: the far one's child 0
        mov.l   r0,@r11
        mov.w   @(4,r9),r0
        mov     r0,r10
5:      bra     .Lwalk
        add     #40,r11

        .align  2
.Leps:  .word   EPS
        .word   0
.Lroom: .long   (L_DEPTH - 1) * 40      ! the last entry's start
.Ldivu: .long   0xFFFFFF00
.Lone:  .long   0x10000
