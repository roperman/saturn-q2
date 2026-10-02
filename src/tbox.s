! The box traces' leaves and brushes in SH-2 assembly (src/trace.c's
! box_leafs_r, and leaf_brushes with clip_box_brush and test_box_brush, which
! they stand in for: the same results, bit for bit).
!
!   int  leafs_asm(box_args *a, int num)
!   void brushes_asm(box_args *a)
!
! leafs_asm: the leaves the box a->c1 .. a->c2 touches, from node num down,
! into a->list (ints): as box_leafs_r, child 0's before child 1's, the first
! MAXLEAFS kept. How many; -1 if its stack of child 1s to come back to ran out
! (the C does it then). A child 1 is kept on that stack, not a call made.
!
! brushes_asm: each leaf of a->list (a->nlist) whose contents meet a->mask,
! each of its brushes whose box meets the move's (a->tbb) not done yet in this
! trace (a->check marked with a->count), and whose contents meet the mask:
! clipped against the move (or, a->test, tested where the box stands), into
! *a->tr. It stops once the fraction's 0, as the C. A BRUSH_EXACT brush's
! first six sides (-x +x -y +y -z +z) are its box's, clipped from that; the
! side entered is read only if it's the trace's. The start and end are taken
! with the box's mins and maxs once a trace (a->sl, sh, el, eh: start + mins,
! start + maxs, end + mins, end + maxs), as the C's sums come out (adding's
! the same in any order, mod 2^32); slanted sides' products are the C's
! fmul, a product at a time, and the fractions the C's frac_div (its divider).

        .text
        .align  2
        .global _leafs_asm
        .global _brushes_asm

A_TBB    = 0                            ! s32: lo x y z, hi x y z (whole units)
A_BOUNDS = 24                           ! lv.brushbounds (6 s16 a brush)
A_CHECK  = 28                           ! brush_check (u16s)
A_COUNT  = 32                           ! checkcount
A_MASK   = 36
A_TR     = 40                           ! &tr
A_LEAFS  = 44
A_LBR    = 48                           ! lv.leafbrushes
A_BRUSH  = 52
A_SIDES  = 56
A_PLANES = 60
A_NODES  = 64
A_POINT  = 68                           ! the box is a point (mins, maxs 0)
A_LIST   = 72
A_NLIST  = 76
A_TEST   = 80
A_SL     = 84                           ! start + mins (x y z); from here r4 in brushes_asm:
V_SL     = 0
V_SH     = 12                           ! start + maxs
V_EL     = 24                           ! end + mins
V_EH     = 36                           ! end + maxs
V_START  = 48
V_END    = 60
V_MINS   = 72
V_MAXS   = 84
A_C1     = 180                          ! leafs_asm's box (16.16)
A_C2     = 192
A_STACK  = 204
STACK_N  = 64
MAXLEAFS = 128

TR_ALLSOLID = 0                         ! q_trace's
TR_STARTSOLID = 4
TR_FRAC  = 8
TR_PLANE = 24
TR_CONT  = 28
TR_SURF  = 32

EPS      = 2048                         ! DIST_EPSILON: 1/32
EXACT    = 0x4000                       ! BRUSH_EXACT

! ---- the leaves ----
!
! r14 a, r13 nodes, r12 planes, r11 the stack's top, r10 num, r9 where the
! next leaf goes, r8 the stack's bottom, r7 &a->c1, r1 the node, r3 its plane,
! r4 its distance, r5 r6 the box's least and most along its normal

_leafs_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        mov     r4,r14
        mov     r5,r10
        mov     #A_NODES,r0
        mov.l   @(r0,r14),r13
        mov.l   @(A_PLANES,r14),r12
        mov     #A_LIST,r0
        mov.l   @(r0,r14),r9
        mov.w   .Lc1,r7
        add     r14,r7
        mov.w   .Lstack,r8
        add     r14,r8
        mov     r8,r11                  ! (nothing kept)

.Lnode:
        cmp/pz  r10
        bf      .Lleaf
        ! the node: nodes + num * 24; its plane: planes + index * 20
        mov     r10,r1
        shll    r1
        add     r10,r1
        shll2   r1
        shll    r1
        add     r13,r1
        mov.w   @r1,r2
        extu.w  r2,r2
        mov     r2,r3
        shll2   r3
        add     r2,r3
        shll2   r3
        add     r12,r3
        mov     #16,r0
        mov.b   @(r0,r3),r0
        extu.b  r0,r0                   ! its type
        mov.l   @(12,r3),r4
        mov     #3,r2
        cmp/hs  r2,r0
        bt      .Lnslant
        ! on an axis: c1[type] - dist, c2[type] - dist
        shll2   r0
        mov.l   @(r0,r7),r5
        add     #12,r0
        mov.l   @(r0,r7),r6
        sub     r4,r5
        bra     .Lnsides
        sub     r4,r6

! fmul(\a, n_j) (\n: the normal's component) added to \acc; \a's register reused. Uses r10
.macro  NFMUL a, n, acc
        dmuls.l \a,\n
        sts     mach,r10
        sts     macl,\a
        xtrct   r10,\a
        add     \a,\acc
.endm

! axis \j: the least corner c1's where the normal's >= 0, else c2's; the most the other's
.macro  NAXIS j
        mov.l   @(4*\j,r3),r0
        mov.l   @(4*\j,r7),r2           ! c1
        cmp/pz  r0
        bt      61f
        mov.l   @(12+4*\j,r7),r2        ! (c2)
61:     NFMUL   r2,r0,r5
        mov.l   @(12+4*\j,r7),r2
        cmp/pz  r0
        bt      62f
        mov.l   @(4*\j,r7),r2
62:     NFMUL   r2,r0,r6
.endm

.Lnslant:
        mov     #0,r5
        mov     #0,r6
        NAXIS   0
        NAXIS   1
        NAXIS   2
        sub     r4,r5
        sub     r4,r6

.Lnsides:
        cmp/pz  r5
        bf      1f
        mov.w   @(2,r1),r0              ! wholly in front: child 0
        bra     .Lnode
        mov     r0,r10
1:      cmp/pz  r6
        bt      2f
        mov.w   @(4,r1),r0              ! wholly behind: child 1
        bra     .Lnode
        mov     r0,r10
2:      ! across it: child 1 kept to come back to, child 0 now
        mov.w   .Lstackn,r2
        add     r8,r2
        cmp/hs  r2,r11
        bt      .Lnfull
        mov.w   @(4,r1),r0
        mov.l   r0,@r11
        add     #4,r11
        mov.w   @(2,r1),r0
        bra     .Lnode
        mov     r0,r10

.Lleaf:
        not     r10,r0                  ! -1 - num
        mov.l   r0,@r9
        add     #4,r9
        mov     #A_LIST,r0
        mov.l   @(r0,r14),r2
        mov.w   .Lmaxl,r0
        add     r0,r2
        cmp/hs  r2,r9
        bt      .Lndone                 ! (as many as kept: the rest the C's way, or not at all)
        cmp/eq  r8,r11
        bt      .Lndone
        add     #-4,r11
        bra     .Lnode
        mov.l   @r11,r10

.Lnfull:
        bra     .Lnout
        mov     #-1,r0
.Lndone:
        mov     #A_LIST,r0
        mov.l   @(r0,r14),r2
        mov     r9,r0
        sub     r2,r0
        shlr2   r0
.Lnout:
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

        .align  2
.Lc1:   .word   A_C1
.Lstack: .word  A_STACK
.Lstackn: .word STACK_N * 4
.Lmaxl: .word   MAXLEAFS * 4

! ---- the brushes ----
!
! The scan: r14 a, r13 the next leaf in the list, r12 the list's end, r11 the
! leaf's next brush (in lv.leafbrushes), r10 its last's end, r9 lv.brushbounds,
! r8 brush_check. A brush clipped: those four kept on the stack, then r4
! &a->sl, r13 the entering fraction, r12 the leaving, r11 the side entered
! (0: none), r10 bit 0 the end out of the solid, bit 1 the start; r9 the side,
! r8 past the last; r7 the brush's box (or the side's plane); the brush on the
! stack's top.

_brushes_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        sts.l   pr,@-r15
        mov     r4,r14
        mov     #A_LIST,r0
        mov.l   @(r0,r14),r13
        mov     #A_NLIST,r0
        mov.l   @(r0,r14),r12
        shll2   r12
        add     r13,r12
        mov.l   @(A_BOUNDS,r14),r9
        mov.l   @(A_CHECK,r14),r8

.Lleafs:
        cmp/hs  r12,r13
        bt      .Lend
        mov.l   @(A_TR,r14),r0
        mov.l   @(TR_FRAC,r0),r0
        tst     r0,r0
        bt      .Lend                   ! a fraction of 0: done
        mov.l   @r13+,r1
        ! the leaf: leafs + n * 28
        mov     r1,r0
        shll2   r0
        shll    r0
        sub     r1,r0
        shll2   r0
        mov.l   @(A_LEAFS,r14),r2
        add     r0,r2
        mov.l   @(24,r2),r0             ! its contents
        mov.l   @(A_MASK,r14),r3
        tst     r3,r0
        bt      .Lleafs
        mov.w   @(20,r2),r0             ! its first brush
        extu.w  r0,r11
        mov.w   @(22,r2),r0             ! how many
        extu.w  r0,r10
        mov.l   @(A_LBR,r14),r0
        shll    r11
        add     r0,r11
        shll    r10
        add     r11,r10

.Lbrushes:
        cmp/hs  r10,r11
        bt      .Lleafs
        mov.w   @r11+,r1
        extu.w  r1,r1                   ! the brush
        mov     r1,r2
        shll    r2
        add     r1,r2
        shll2   r2
        add     r9,r2                   ! its box
        ! off the move's box: nothing to clip (nor the brush read)
        mov.w   @(0,r2),r0
        mov.l   @(12,r14),r3
        cmp/gt  r3,r0
        bt      .Lbrushes
        mov.w   @(6,r2),r0
        mov.l   @(0,r14),r3
        cmp/gt  r0,r3
        bt      .Lbrushes
        mov.w   @(2,r2),r0
        mov.l   @(16,r14),r3
        cmp/gt  r3,r0
        bt      .Lbrushes
        mov.w   @(8,r2),r0
        mov.l   @(4,r14),r3
        cmp/gt  r0,r3
        bt      .Lbrushes
        mov.w   @(4,r2),r0
        mov.l   @(20,r14),r3
        cmp/gt  r3,r0
        bt      .Lbrushes
        mov.w   @(10,r2),r0
        mov.l   @(8,r14),r3
        cmp/gt  r0,r3
        bt      .Lbrushes
        ! done already from another leaf? Marked
        mov     r1,r0
        add     r0,r0
        mov.w   @(r0,r8),r3
        mov.l   @(A_COUNT,r14),r4
        extu.w  r3,r3
        cmp/eq  r4,r3
        bt      .Lbrushes
        mov.w   r4,@(r0,r8)
        ! the brush: brushes + n * 8; its contents to meet the mask, and sides
        shll2   r1
        shll    r1
        mov.l   @(A_BRUSH,r14),r3
        add     r1,r3
        mov.l   @r3,r0
        mov.l   @(A_MASK,r14),r4
        tst     r4,r0
        bt      .Lbrushes
        mov.w   @(6,r3),r0
        extu.w  r0,r0
        tst     r0,r0
        bt      .Lbrushes

        ! ---- the brush (r3), its box r2, r0 its sides ----
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r3,@-r15
        mov     r2,r7
        mov     r0,r8
        mov.w   @(4,r3),r0
        extu.w  r0,r9
        mov.l   @(A_SIDES,r14),r0
        shll2   r9
        add     r0,r9                   ! its first side
        shll2   r8
        add     r9,r8                   ! past its last
        mov     #A_SL,r4
        add     r14,r4
        mov.l   .Lminus1,r13            ! entering: -1
        mov.l   .Lone,r12               ! leaving: 1
        mov     #0,r11
        mov     #0,r10
        mov     #A_TEST,r0
        mov.l   @(r0,r14),r0
        tst     r0,r0
        bf      .Ltest
        mov.l   @r3,r0
        mov.w   .Lexact,r1
        tst     r1,r0
        bt      .Lsides

! the box's side on axis \j: its least (-), d1 = box's - (start + maxs), d2 = box's - (end + maxs)
.macro  XNEG j
        mov.w   @(2*\j,r7),r0
        shll16  r0
        mov.l   @(V_SH+4*\j,r4),r1
        mov.l   @(V_EH+4*\j,r4),r2
        mov     r0,r5
        sub     r1,r5
        mov     r0,r6
        bsr     .Lside
        sub     r2,r6
        add     #4,r9
.endm

! its most (+): d1 = (start + mins) - box's, d2 = (end + mins) - box's
.macro  XPOS j
        mov.w   @(6+2*\j,r7),r0
        shll16  r0
        mov.l   @(V_SL+4*\j,r4),r5
        mov.l   @(V_EL+4*\j,r4),r6
        sub     r0,r5
        bsr     .Lside
        sub     r0,r6
        add     #4,r9
.endm

        ! a BRUSH_EXACT brush: its box's six sides from its box
        XNEG    0
        XPOS    0
        XNEG    1
        XPOS    1
        XNEG    2
        XPOS    2

.Lsides:
        cmp/hs  r8,r9
        bt      .Lsidesdone
        bsr     .Lside_d
        nop
        bsr     .Lside
        nop
        bra     .Lsides
        add     #4,r9

.Lsidesdone:
        mov     r10,r0
        tst     #2,r0
        bf      .Lout
        ! the start was inside it
        mov.l   @(A_TR,r14),r2
        mov     #1,r1
        mov.l   r1,@(TR_STARTSOLID,r2)
        tst     #1,r0
        bf      .Lnohit
        bra     .Lnohit
        mov.l   r1,@(TR_ALLSOLID,r2)
.Lout:
        ! entered before it's left, after -1, and nearer than the nearest yet: the nearest now
        cmp/ge  r12,r13
        bt      .Lnohit
        mov.l   .Lminus1,r0
        cmp/gt  r0,r13
        bf      .Lnohit
        mov.l   @(A_TR,r14),r2
        mov.l   @(TR_FRAC,r2),r0
        cmp/gt  r13,r0
        bf      .Lnohit
        cmp/pz  r13
        bt      1f
        mov     #0,r13
1:      mov.l   r13,@(TR_FRAC,r2)
        mov.w   @r11,r0                 ! the side entered: its plane
        extu.w  r0,r1
        mov     r1,r3
        shll2   r3
        add     r1,r3
        shll2   r3
        mov.l   @(A_PLANES,r14),r0
        add     r0,r3
        mov.l   r3,@(TR_PLANE,r2)
        mov.w   @(2,r11),r0             ! its flags
        extu.w  r0,r0
        mov.l   r0,@(TR_SURF,r2)
        mov.l   @r15,r3
        mov.l   @r3,r1                  ! the brush's contents (but BRUSH_EXACT)
        mov.w   .Lexact,r0
        not     r0,r0
        and     r0,r1
        mov.l   r1,@(TR_CONT,r2)

.Lnohit:
        add     #4,r15
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @(A_BOUNDS,r14),r9
        mov.l   @(A_CHECK,r14),r8
        mov.l   @(A_TR,r14),r0
        mov.l   @(TR_FRAC,r0),r0
        tst     r0,r0
        bf      .Lbrushes               ! (a fraction of 0: done)

.Lend:
        lds.l   @r15+,pr
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

        .align  2
.Lexact: .word  EXACT
        .word   0
.Lminus1: .long -0x10000
.Lone:  .long   0x10000

! ---- where the box stands (a->test): in it only if it's behind every side ----
.Ltest:
        mov.l   @r3,r0
        mov.w   .Lexact2,r1
        tst     r1,r0
        bt      .Ltsides
        ! the box's six from its box: d1 alone
        mov.w   @(0,r7),r0
        shll16  r0
        mov.l   @(V_SH,r4),r1
        sub     r1,r0
        cmp/pl  r0
        bt      .Lnohit
        mov.w   @(6,r7),r0
        shll16  r0
        mov.l   @(V_SL,r4),r1
        sub     r0,r1
        cmp/pl  r1
        bt      .Lnohit
        mov.w   @(2,r7),r0
        shll16  r0
        mov.l   @(V_SH+4,r4),r1
        sub     r1,r0
        cmp/pl  r0
        bt      .Lnohit
        mov.w   @(8,r7),r0
        shll16  r0
        mov.l   @(V_SL+4,r4),r1
        sub     r0,r1
        cmp/pl  r1
        bt      .Lnohit
        mov.w   @(4,r7),r0
        shll16  r0
        mov.l   @(V_SH+8,r4),r1
        sub     r1,r0
        cmp/pl  r0
        bt      .Lnohit
        mov.w   @(10,r7),r0
        shll16  r0
        mov.l   @(V_SL+8,r4),r1
        sub     r0,r1
        cmp/pl  r1
        bt      .Lnohit
        add     #24,r9
.Ltsides:
        cmp/hs  r8,r9
        bt      .Ltsolid
        bsr     .Lside_d
        nop
        cmp/pl  r5
        bt      .Lnohit
        bra     .Ltsides
        add     #4,r9
.Ltsolid:
        mov.l   @(A_TR,r14),r2
        mov     #1,r1
        mov.l   r1,@(TR_STARTSOLID,r2)
        mov.l   r1,@(TR_ALLSOLID,r2)
        mov     #0,r1
        mov.l   r1,@(TR_FRAC,r2)
        mov.l   @r15,r3
        mov.l   @r3,r1
        mov.w   .Lexact2,r0
        not     r0,r0
        and     r0,r1
        bra     .Lnohit
        mov.l   r1,@(TR_CONT,r2)

        .align  2
.Lexact2: .word EXACT

! ---- a side read (r9): r5 d1, r6 d2. Uses r0-r3, r7 ----
.Lside_d:
        mov.w   @r9,r0
        extu.w  r0,r1
        mov     r1,r7
        shll2   r7
        add     r1,r7
        shll2   r7
        mov.l   @(A_PLANES,r14),r0
        add     r0,r7                   ! its plane
        mov     #16,r0
        mov.b   @(r0,r7),r0
        extu.b  r0,r0
        mov.l   @(12,r7),r1             ! its distance
        mov     #3,r2
        cmp/hs  r2,r0
        bt      .Lslant
        ! on an axis: + (n > 0) d1 = (start + mins) - dist; - d1 = -(start + maxs) - dist
        shll2   r0
        mov.l   @(r0,r7),r2
        cmp/pl  r2
        bf      1f
        mov.l   @(r0,r4),r5
        add     #V_EL,r0
        mov.l   @(r0,r4),r6
        sub     r1,r5
        rts
        sub     r1,r6
1:      add     #V_SH,r0
        mov.l   @(r0,r4),r5
        add     #V_EH-V_SH,r0
        mov.l   @(r0,r4),r6
        neg     r5,r5
        sub     r1,r5
        neg     r6,r6
        rts
        sub     r1,r6

! fmul(\a, \b) into \out (the C's: (s64)a * b >> 16). Uses \t
.macro  FMUL a, b, out, t
        dmuls.l \a,\b
        sts     mach,\t
        sts     macl,\out
        xtrct   \t,\out
.endm

.Lslant:
        ! pushed out for the box (not a point): dist -= ofs . n, ofs each axis's maxs where n < 0, else mins
        mov     #A_POINT,r0
        mov.l   @(r0,r14),r0
        tst     r0,r0
        bf      3f
        mov.l   @r7,r2
        mov     #V_MINS,r0
        cmp/pz  r2
        bt      2f
        mov     #V_MAXS,r0
2:      mov.l   @(r0,r4),r3
        FMUL    r3,r2,r5,r6
        sub     r5,r1
        mov.l   @(4,r7),r2
        mov     #V_MINS+4,r0
        cmp/pz  r2
        bt      2f
        mov     #V_MAXS+4,r0
2:      mov.l   @(r0,r4),r3
        FMUL    r3,r2,r5,r6
        sub     r5,r1
        mov.l   @(8,r7),r2
        mov     #V_MINS+8,r0
        cmp/pz  r2
        bt      2f
        mov     #V_MAXS+8,r0
2:      mov.l   @(r0,r4),r3
        FMUL    r3,r2,r5,r6
        sub     r5,r1
3:      ! d1 = start . n - dist, d2 = end . n - dist
        mov.l   @(V_START,r4),r3
        mov.l   @r7,r2
        FMUL    r3,r2,r5,r6
        mov.l   @(V_START+4,r4),r3
        mov.l   @(4,r7),r2
        FMUL    r3,r2,r0,r6
        add     r0,r5
        mov.l   @(V_START+8,r4),r3
        mov.l   @(8,r7),r2
        FMUL    r3,r2,r0,r6
        add     r0,r5
        sub     r1,r5
        mov.l   @(V_END,r4),r3
        mov.l   @r7,r2
        FMUL    r3,r2,r6,r0
        mov     #V_END+4,r0
        mov.l   @(r0,r4),r3
        mov.l   @(4,r7),r2
        dmuls.l r3,r2
        sts     mach,r0
        sts     macl,r3
        xtrct   r0,r3
        add     r3,r6
        mov     #V_END+8,r0
        mov.l   @(r0,r4),r3
        mov.l   @(8,r7),r2
        dmuls.l r3,r2
        sts     mach,r0
        sts     macl,r3
        xtrct   r0,r3
        add     r3,r6
        rts
        sub     r1,r6

! n (r5) / d (r1 > 0), 16.16, kept to +-2 (the C's frac_div): r0. Uses r2 r3
.macro  FRAC
        mov     r1,r2
        add     r2,r2                   ! 2d
        cmp/ge  r2,r5
        bt      81f                     ! n >= 2d (so n > 0): 2
        neg     r2,r2
        cmp/ge  r5,r2
        bt      82f                     ! n <= -2d: -2
        mov.l   .Ldivu,r3
        mov.l   r1,@r3                  ! DVSR
        mov     r5,r0
        shlr16  r0
        exts.w  r0,r0                   ! n >> 16
        mov.l   r0,@(16,r3)             ! DVDNTH
        shll16  r5
        mov.l   r5,@(20,r3)             ! DVDNTL: it starts
        bra     83f
        mov.l   @(20,r3),r0             ! (waits for it)
81:     mov.l   .Ltwo,r0                ! (not in the branch's slot: a PC-relative load there is off)
        bra     83f
        nop
82:     mov.l   .Lmtwo,r0
83:
.endm

! ---- a side's part: r5 d1, r6 d2 (the start's and the end's distance in front of it,
! pushed out for the box), r9 the side. If the box is wholly in front of it, no hit: on to
! .Lnohit (this call's not come back from). Uses r0-r3 ----
.Lside:
        cmp/pl  r6
        bf      1f
        mov     #1,r0
        or      r0,r10                  ! the end's out of the solid
1:      cmp/pl  r5
        bf      .Lsback
        mov     #2,r0
        or      r0,r10                  ! the start's out
        cmp/ge  r5,r6
        bf      .Lsenter
        bra     .Lnohit                 ! wholly in front of it
        nop
.Lsenter:
        ! entering: f = frac_div(d1 - EPS, d1 - d2); the latest kept
        mov     r5,r1
        sub     r6,r1
        mov.w   .Leps,r0
        sub     r0,r5
        FRAC
        cmp/gt  r13,r0
        bf      2f
        mov     r0,r13
        mov     r9,r11
2:      rts
        nop
.Lsback:
        cmp/pl  r6
        bt      .Lsleave
        rts                             ! wholly behind it: nothing
        nop
.Lsleave:
        ! leaving: f = frac_div(d1 + EPS, d1 - d2), n and d turned positive; the earliest kept
        mov     r6,r1
        sub     r5,r1
        mov.w   .Leps,r0
        add     r0,r5
        neg     r5,r5
        FRAC
        cmp/gt  r0,r12
        bf      3f
        mov     r0,r12
3:      rts
        nop

        .align  2
.Leps:  .word   EPS
        .word   0
.Ltwo:  .long   0x20000
.Lmtwo: .long   -0x20000
.Ldivu: .long   0xFFFFFF00
