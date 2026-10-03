! A face's setup, in SH-2 assembly (src/render.c's face_setup, which it
! stands in for, bit for bit): where its grid starts and its steps in view
! space, whether any of it's in view, its coarse grid if it's far, and the
! whole grid at once if it fits (grid_face_asm).
!
!   int face_asm(face_args *a, int fi, int model)
!
! 0: nothing to draw (the sky); 1: out of view; 2: the steps and the whole
! grid done; 3: the steps done, the grid too big to do at once (the C does it
! a row at a time). The results go where the C's do: a (fo .. light), a->ga
! and a->gk.
!
! What makes it quicker than the compiler's: the frame's constants read
! straight from a (GBR); the face's corners tested for the view one at a
! time, stopping at the first in it (most faces: the first); the lowest z
! from the steps' signs; the results stored as they come, not built on the
! stack and passed on.
!
! GBR points at a (its constants: R0 only), r14 too (its results, at small
! offsets); r13 the face (then its coarse grid's record).

        .text
        .align  2
        .global _face_asm

A_FO      = 0                           ! (face_args) the results
A_FNU     = 12
A_FNV     = 16
A_DUT     = 20
A_DVT     = 32
A_EU0     = 44
A_EU1     = 48
A_EV0     = 52
A_EV1     = 56
A_CELLS   = 60
A_LIGHT   = 64
A_RT      = 68                          ! the frame's
A_UP      = 80
A_FW      = 92
A_POS     = 104
A_KY      = 116
A_LODZ    = 120
A_RCPN    = 124
A_FRAME   = 128
A_MOVER   = 132
A_AXV     = 136
A_AXES    = 140
A_FACES   = 144
A_CELLS0  = 148
A_LODCELLS = 152
A_LIGHTS0 = 156
A_LODLIGHTS = 160
A_LODFACES = 164
A_N       = 168
A_GRID    = 172
A_GA      = 176
A_GK      = 180
A_PRX0    = 184                         ! the face's cluster's rectangle (portal_face): its
A_PRX1    = 188                         ! sides' slopes, if not the whole screen
A_PRY0    = 192
A_PRY1    = 196
A_PRECT   = 200

F_AXES    = 12                          ! (q_face) u16
F_FLAGS   = 16                          ! then nu nv eu0 eu1 ev0 ev1 lodhi (u8)
F_CELL    = 24
F_LIGHT   = 28                          ! (the top byte: the coarse grid's number, low)

GA_P      = 0                           ! (grid_args)
GA_E0     = 12
GA_D      = 24
GA_E1     = 36
GK_F0     = 44                          ! (gk, bytes) f0 dv f1
GK_ROWS   = 36

FF_SKIP   = 33                          ! FF_SKY | FF_NODRAW
FF_LOD    = 128

! fmul(\a, \b) into \out (the C's: (s64)a * b >> 16). Uses \t
.macro  FMUL a, b, out, t
        dmuls.l \a,\b
        sts     mach,\t
        sts     macl,\out
        xtrct   \t,\out
.endm

! \out = r8 r9 r10 . the camera's axis at \ax (each product truncated, as the C's
! to_view). Uses r0 r2 r3
.macro  VIEW ax, out
        mov.l   @(\ax,gbr),r0
        dmuls.l r0,r8
        mov.l   @(\ax+4,gbr),r0
        sts     mach,r2
        sts     macl,\out
        dmuls.l r0,r9
        xtrct   r2,\out
        mov.l   @(\ax+8,gbr),r0
        sts     mach,r2
        sts     macl,r3
        dmuls.l r0,r10
        xtrct   r2,r3
        add     r3,\out
        sts     mach,r2
        sts     macl,r3
        xtrct   r2,r3
        add     r3,\out
.endm

! which of the view's planes the point \x \y \z is outside, into \out (the C's
! view_oc: the bits in another order, which doesn't matter for ANDing). Uses r0 r2 r3
.macro  OC x, y, z, out
        mov.l   @(A_KY,gbr),r0
        dmuls.l r0,\z
        mov     #8,r2
        shll16  r2                      ! NEAR_Z
        cmp/gt  \z,r2                   ! z < NEAR_Z
        movt    \out
        neg     \z,r0
        cmp/gt  \x,r0                   ! x < -z: left
        rotcl   \out
        cmp/gt  \z,\x                   ! x > z: right
        rotcl   \out
        sts     mach,r2
        sts     macl,r3
        xtrct   r2,r3                   ! ty = fmul(z, ky)
        cmp/gt  r3,\y                   ! y > ty: top
        rotcl   \out
        neg     r3,r3
        cmp/gt  \y,r3                   ! y < -ty: bottom
        rotcl   \out
.endm

! which of the sides of the face's cluster's rectangle (A_PRX0 ..) the point \x \y \z is
! outside, into \out (the C's rect_oc). Uses r0 r2 r3
.macro  RC x, y, z, out
        mov.l   @(A_PRX0,gbr),r0
        dmuls.l r0,\z
        mov.l   @(A_PRX1,gbr),r0
        sts     mach,r2
        sts     macl,r3
        dmuls.l r0,\z
        xtrct   r2,r3                   ! fmul(z, prx0)
        cmp/gt  \x,r3                   ! x < it: left
        movt    \out
        sts     mach,r2
        sts     macl,r3
        mov.l   @(A_PRY1,gbr),r0
        dmuls.l r0,\z
        xtrct   r2,r3                   ! fmul(z, prx1)
        cmp/gt  r3,\x                   ! x > it: right
        rotcl   \out
        sts     mach,r2
        sts     macl,r3
        mov.l   @(A_PRY0,gbr),r0
        dmuls.l r0,\z
        xtrct   r2,r3                   ! fmul(z, pry1)
        cmp/gt  r3,\y                   ! y > it: top
        rotcl   \out
        sts     mach,r2
        sts     macl,r3
        xtrct   r2,r3                   ! fmul(z, pry0)
        cmp/gt  \y,r3                   ! y < it: bottom
        rotcl   \out
.endm

! a stored texel along an axis, t = fmul(@(\soff,\s), r1) to @(\toff,r14), and the
! steps from it: @(\aoff,\s) = t * \ka, @(\boff,\s) = t * \kb; the three components
! (soff, +4, +8) interleaved so that no multiply's result is read as soon as it's
! asked for. Uses r0 r4 r13 and \t1 \t2
.macro  STEPS3 s, soff, toff, aoff, ka, boff, kb, t1, t2
        mov.l   @(\soff,\s),r0
        dmuls.l r0,r1
        mov.l   @(\soff+4,\s),r0
        sts     mach,r13
        sts     macl,r4
        dmuls.l r0,r1
        xtrct   r13,r4                  ! t0
        mov.l   @(\soff+8,\s),r0
        sts     mach,r13
        sts     macl,\t1
        dmuls.l r0,r1
        xtrct   r13,\t1                 ! t1
        mov.l   r4,@(\toff,r14)
        mov.l   \t1,@(\toff+4,r14)
        sts     mach,r13
        sts     macl,\t2
        xtrct   r13,\t2                 ! t2
        mov     r4,r0
        mul.l   \ka,r0
        mov     \t1,r13
        mov.l   \t2,@(\toff+8,r14)
        sts     macl,r0
        mul.l   \ka,r13
        mov.l   r0,@(\aoff,\s)
        mov     \t2,r0
        sts     macl,r13
        mul.l   \ka,r0
        mov.l   r13,@(\aoff+4,\s)
        mov     r4,r13
        sts     macl,r0
        mul.l   \kb,r13
        mov.l   r0,@(\aoff+8,\s)
        mov     \t1,r0
        sts     macl,r13
        mul.l   \kb,r0
        mov.l   r13,@(\boff,\s)
        mov     \t2,r13
        sts     macl,r0
        mul.l   \kb,r13
        mov.l   r0,@(\boff+4,\s)
        nop
        sts     macl,r13
        mov.l   r13,@(\boff+8,\s)
.endm

_face_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        sts.l   pr,@-r15
        sts.l   mach,@-r15
        sts.l   macl,@-r15
        stc.l   gbr,@-r15
        ldc     r4,gbr
        mov     r4,r14
        ! the face: faces + fi x 32
        mov.l   @(A_FACES,gbr),r0
        shll2   r5
        shll2   r5
        shll    r5
        add     r0,r5
        mov     r5,r13
        mov     #F_FLAGS,r0
        mov.b   @(r0,r13),r0
        tst     #FF_SKIP,r0
        bt      1f
        bra     .Lret                   ! the sky, or not drawn
        mov     #0,r0
1:      ! d = its origin + its model's offset - the camera (r8 r9 r10)
        mov.l   @(A_MOVER,gbr),r0
        mov     r6,r1
        shll    r1
        add     r6,r1
        shll2   r1
        add     r0,r1                   ! mover_ofs[model]
        mov.l   @r13,r8
        mov.l   @r1,r0
        add     r0,r8
        mov.l   @(A_POS,gbr),r0
        sub     r0,r8
        mov.l   @(4,r13),r9
        mov.l   @(4,r1),r0
        add     r0,r9
        mov.l   @(A_POS+4,gbr),r0
        sub     r0,r9
        mov.l   @(8,r13),r10
        mov.l   @(8,r1),r0
        add     r0,r10
        mov.l   @(A_POS+8,gbr),r0
        sub     r0,r10
        ! o: into view space, aside in fo
        VIEW    A_RT,r4
        VIEW    A_UP,r5
        VIEW    A_FW,r6
        mov.l   r4,@(A_FO,r14)
        mov.l   r5,@(A_FO+4,r14)
        mov.l   r6,@(A_FO+8,r14)
        ! its axes in view space (du into ga->d, dv into gk's): this frame's if they've been
        ! done (both CPUs share them: a vector and its frame fill a cache line), else done now
        mov.l   @(A_GA,gbr),r0
        mov     r0,r11
        mov.l   @(A_GK,gbr),r0
        add     #GK_F0,r0
        mov     r0,r12
        mov.w   @(F_AXES,r13),r0
        extu.w  r0,r1                   ! the pair
        mov     r1,r7
        shll2   r7
        shll2   r7
        shll    r7
        mov.l   @(A_AXV,gbr),r0
        add     r0,r7                   ! axis_view + pair x 32
        mov.l   @(A_FRAME,gbr),r0
        mov     r0,r3
        mov.l   @(12,r7),r2
        cmp/eq  r3,r2
        bf      .Laxes
        mov.l   @(28,r7),r2
        cmp/eq  r3,r2
        bf      .Laxes
        mov.l   @r7,r0
        mov.l   r0,@(GA_D,r11)
        mov.l   @(4,r7),r0
        mov.l   r0,@(GA_D+4,r11)
        mov.l   @(8,r7),r0
        mov.l   r0,@(GA_D+8,r11)
        mov.l   @(16,r7),r0
        mov.l   r0,@(12,r12)
        mov.l   @(20,r7),r0
        mov.l   r0,@(16,r12)
        mov.l   @(24,r7),r0
        bra     .Lcull
        mov.l   r0,@(20,r12)

.Laxes:
        ! axes + pair x 24: du, then dv
        shll2   r1
        shll    r1
        mov     r1,r0
        add     r1,r1
        add     r0,r1
        mov.l   @(A_AXES,gbr),r0
        add     r0,r1
        mov.l   @r1,r8
        mov.l   @(4,r1),r9
        mov.l   @(8,r1),r10
        VIEW    A_RT,r4
        VIEW    A_UP,r5
        VIEW    A_FW,r6
        mov.l   r4,@r7
        mov.l   r5,@(4,r7)
        mov.l   r6,@(8,r7)
        mov.l   @(A_FRAME,gbr),r0
        mov.l   r0,@(12,r7)
        mov.l   r4,@(GA_D,r11)
        mov.l   r5,@(GA_D+4,r11)
        mov.l   r6,@(GA_D+8,r11)
        mov.l   @(12,r1),r8
        mov.l   @(16,r1),r9
        mov.l   @(20,r1),r10
        VIEW    A_RT,r4
        VIEW    A_UP,r5
        VIEW    A_FW,r6
        mov.l   r4,@(16,r7)
        mov.l   r5,@(20,r7)
        mov.l   r6,@(24,r7)
        mov.l   @(A_FRAME,gbr),r0
        mov.l   r0,@(28,r7)
        mov.l   r4,@(12,r12)
        mov.l   r5,@(16,r12)
        mov.l   r6,@(20,r12)

.Lcull:
        ! all of it outside one of the view's planes? Its grid's a parallelogram: o, u =
        ! du x nu (r7 r8 r9), v = dv x nv (r10 r11 r12); its four corners' outcodes ANDed,
        ! one at a time until they come to nothing (r1)
        mov     #F_FLAGS+1,r0
        mov.b   @(r0,r13),r1
        extu.b  r1,r1                   ! nu
        mov     #F_FLAGS+2,r0
        mov.b   @(r0,r13),r2
        extu.b  r2,r2                   ! nv
        mov.l   @(GA_D,r11),r7
        mul.l   r1,r7
        sts     macl,r7
        mov.l   @(GA_D+4,r11),r8
        mul.l   r1,r8
        sts     macl,r8
        mov.l   @(GA_D+8,r11),r9
        mul.l   r1,r9
        sts     macl,r9
        mov.l   @(12,r12),r10
        mul.l   r2,r10
        sts     macl,r10
        mov.l   @(16,r12),r11
        mul.l   r2,r11
        sts     macl,r11
        mov.l   @(20,r12),r12
        mul.l   r2,r12
        sts     macl,r12
        mov.l   @(A_FO,r14),r4
        mov.l   @(A_FO+4,r14),r5
        mov.l   @(A_FO+8,r14),r6
        mov.l   r13,@-r15
        OC      r4,r5,r6,r1             ! o
        tst     r1,r1
        bt      .Lseen
        add     r7,r4
        add     r8,r5
        add     r9,r6
        OC      r4,r5,r6,r13            ! o + u
        and     r13,r1
        tst     r1,r1
        bt      .Lseen
        add     r10,r4
        add     r11,r5
        add     r12,r6
        OC      r4,r5,r6,r13            ! o + u + v
        and     r13,r1
        tst     r1,r1
        bt      .Lseen
        sub     r7,r4
        sub     r8,r5
        sub     r9,r6
        OC      r4,r5,r6,r13            ! o + v
        and     r13,r1
        tst     r1,r1
        bt      .Lseen
        mov.l   @r15+,r13
        bra     .Lret                   ! out of view
        mov     #1,r0

.Lseen:
        ! through the portals: the same for the face's cluster's rectangle, if it has one
        mov.l   @(A_PRECT,gbr),r0
        tst     r0,r0
        bf      2f
        bra     .Lrin                   ! (too far for BT)
        nop
2:
        mov.l   @(A_FO,r14),r4
        mov.l   @(A_FO+4,r14),r5
        mov.l   @(A_FO+8,r14),r6
        RC      r4,r5,r6,r1             ! o
        tst     r1,r1
        bf      1f
        bra     .Lrin                   ! (too far for BT)
        nop
1:
        add     r7,r4
        add     r8,r5
        add     r9,r6
        RC      r4,r5,r6,r13            ! o + u
        and     r13,r1
        tst     r1,r1
        bt      .Lrin
        add     r10,r4
        add     r11,r5
        add     r12,r6
        RC      r4,r5,r6,r13            ! o + u + v
        and     r13,r1
        tst     r1,r1
        bt      .Lrin
        sub     r7,r4
        sub     r8,r5
        sub     r9,r6
        RC      r4,r5,r6,r13            ! o + v
        and     r13,r1
        tst     r1,r1
        bt      .Lrin
        mov.l   @r15+,r13
        bra     .Lret                   ! not seen through them
        mov     #1,r0
.Lrin:
        mov.l   @r15+,r13
        ! the nearest corner's z: o's, plus u's and v's where they come nearer
        mov.l   @(A_FO+8,r14),r4
        cmp/pz  r9
        bt      1f
        add     r9,r4
1:      cmp/pz  r12
        bt      2f
        add     r12,r4
2:      ! far: its coarse grid, if it has one
        mov     #F_FLAGS,r0
        mov.b   @(r0,r13),r0
        tst     #FF_LOD,r0
        bt      .Lface
        mov.l   @(A_LODZ,gbr),r0
        cmp/gt  r0,r4
        bt      .Llod

.Lface:
        ! its own grid: nu nv (r11 r12), eu0 eu1 ev0 ev1 (r7 .. r10), its cells and lights
        mov     #F_FLAGS+1,r0
        mov.b   @(r0,r13),r11
        extu.b  r11,r11
        mov     #F_FLAGS+2,r0
        mov.b   @(r0,r13),r12
        extu.b  r12,r12
        mov     #F_FLAGS+3,r0
        mov.b   @(r0,r13),r7
        extu.b  r7,r7
        mov     #F_FLAGS+4,r0
        mov.b   @(r0,r13),r8
        extu.b  r8,r8
        mov     #F_FLAGS+5,r0
        mov.b   @(r0,r13),r9
        extu.b  r9,r9
        mov     #F_FLAGS+6,r0
        mov.b   @(r0,r13),r10
        extu.b  r10,r10
        mov.l   @(F_CELL,r13),r1
        shll2   r1
        shll    r1
        mov.l   @(A_CELLS0,gbr),r0
        add     r0,r1
        mov.l   r1,@(A_CELLS,r14)
        mov.l   @(F_LIGHT,r13),r1
        shll8   r1
        shlr8   r1                      ! (the low 24 bits)
        add     r1,r1
        mov.l   @(A_LIGHTS0,gbr),r0
        add     r1,r0
        bra     .Lsteps
        mov.l   r0,@(A_LIGHT,gbr)

.Llod:
        ! its record: lodfaces + (lodhi << 8 | the first light's top byte) x 16
        mov     #F_FLAGS+7,r0
        mov.b   @(r0,r13),r1
        extu.b  r1,r1
        shll8   r1
        mov     #F_LIGHT,r0
        mov.b   @(r0,r13),r0
        extu.b  r0,r0
        or      r0,r1
        shll2   r1
        shll2   r1
        mov.l   @(A_LODFACES,gbr),r0
        add     r0,r1
        mov     r1,r13
        ! its first point offu, offv texels back (x 65536 / 32: in the face's cells), its
        ! axes doubled
        mov.b   @(6,r13),r0
        shll8   r0
        shll2   r0
        shll    r0
        mov     r0,r1                   ! ou
        mov.b   @(7,r13),r0
        shll8   r0
        shll2   r0
        shll    r0
        mov     r0,r2                   ! ov
        mov.l   @(A_GA,gbr),r0
        mov     r0,r3
        mov.l   @(A_GK,gbr),r0
        add     #GK_F0,r0
        mov     r0,r5
        mov.l   @(GA_D,r3),r7
        mov.l   @(12,r5),r8
        FMUL    r7,r1,r9,r10
        FMUL    r8,r2,r11,r10
        add     r11,r9
        mov.l   @(A_FO,r14),r0
        sub     r9,r0
        mov.l   r0,@(A_FO,r14)
        add     r7,r7
        mov.l   r7,@(GA_D,r3)
        add     r8,r8
        mov.l   r8,@(12,r5)
        mov.l   @(GA_D+4,r3),r7
        mov.l   @(16,r5),r8
        FMUL    r7,r1,r9,r10
        FMUL    r8,r2,r11,r10
        add     r11,r9
        mov.l   @(A_FO+4,r14),r0
        sub     r9,r0
        mov.l   r0,@(A_FO+4,r14)
        add     r7,r7
        mov.l   r7,@(GA_D+4,r3)
        add     r8,r8
        mov.l   r8,@(16,r5)
        mov.l   @(GA_D+8,r3),r7
        mov.l   @(20,r5),r8
        FMUL    r7,r1,r9,r10
        FMUL    r8,r2,r11,r10
        add     r11,r9
        mov.l   @(A_FO+8,r14),r0
        sub     r9,r0
        mov.l   r0,@(A_FO+8,r14)
        add     r7,r7
        mov.l   r7,@(GA_D+8,r3)
        add     r8,r8
        mov.l   r8,@(20,r5)
        ! nu nv (r11 r12), eu0 eu1 ev0 ev1 (r7 .. r10), its cells and lights
        mov.b   @(0,r13),r0
        extu.b  r0,r11
        mov.b   @(1,r13),r0
        extu.b  r0,r12
        mov.b   @(2,r13),r0
        extu.b  r0,r7
        mov.b   @(3,r13),r0
        extu.b  r0,r8
        mov.b   @(4,r13),r0
        extu.b  r0,r9
        mov.b   @(5,r13),r0
        extu.b  r0,r10
        mov.l   @(8,r13),r1
        shll2   r1
        shll    r1
        mov.l   @(A_LODCELLS,gbr),r0
        add     r0,r1
        mov.l   r1,@(A_CELLS,r14)
        mov.l   @(12,r13),r1
        add     r1,r1
        mov.l   @(A_LODLIGHTS,gbr),r0
        add     r1,r0
        mov.l   r0,@(A_LIGHT,gbr)

.Lsteps:
        mov.l   r11,@(A_FNU,r14)
        mov.l   r12,@(A_FNV,r14)
        mov.l   r7,@(A_EU0,r14)
        mov.l   r8,@(A_EU1,r14)
        mov.l   r9,@(A_EV0,r14)
        mov.l   r10,@(A_EV1,r14)
        mov.l   @(A_GA,gbr),r0
        mov     r0,r3                   ! ga
        mov.l   @(A_GK,gbr),r0
        add     #GK_F0,r0
        mov     r0,r5                   ! gk's f0 dv f1
        mov.l   @(A_RCPN,gbr),r0
        mov     r0,r1
        mov.l   @(A_N,gbr),r0
        mov     r0,r6                   ! N
        ! along u: a stored texel (dut), the steps out of the first column ((nu == 1 ? eu1 :
        ! N) - eu0 of them) and into the last (eu1)
        mov     r6,r2
        mov     r11,r0
        cmp/eq  #1,r0
        bf      1f
        mov     r8,r2
1:      sub     r7,r2
        STEPS3  r3,GA_D,A_DUT,GA_E0,r2,GA_E1,r8,r7,r11      ! (r7 eu0 and r11 nu as scratch: stored above)
        ! along v: dvt, and the steps out of the first row and into the last (f0, f1)
        mov     r6,r2
        mov     r12,r0
        cmp/eq  #1,r0
        bf      2f
        mov     r10,r2
2:      sub     r9,r2
        STEPS3  r5,12,A_DVT,0,r2,24,r10,r7,r9
        mov.l   @(A_FNU,r14),r11        ! (nu back)
        ! the grid's first point
        mov.l   @(A_FO,r14),r0
        mov.l   r0,@(GA_P,r3)
        mov.l   @(A_FO+4,r14),r0
        mov.l   r0,@(GA_P+4,r3)
        mov.l   @(A_FO+8,r14),r0
        mov.l   r0,@(GA_P+8,r3)
        ! the whole grid at once if it fits ((nu + 1) x (nv + 1) points, 2 x MAX_ROW)
        mov     r11,r0
        add     #1,r0
        mov     r12,r4
        add     #1,r4
        mul.l   r0,r4
        sts     macl,r4
        mov.w   .Lmaxpts,r0
        cmp/gt  r0,r4
        bf      3f
        bra     .Lret                   ! too big: a row at a time
        mov     #3,r0
3:      mov     r5,r0
        add     #GK_ROWS-GK_F0,r0
        mov.l   r12,@r0                 ! the rows after the first
        mov.l   @(A_GRID,gbr),r0
        mov     r0,r4
        mov     r11,r5
        add     #1,r5
        mov     r3,r6
        mov.l   @(A_GK,gbr),r0
        mov     r0,r7
        mov.l   .Lgrid_face,r0
        jsr     @r0
        nop
        mov     #2,r0

.Lret:
        ldc.l   @r15+,gbr
        lds.l   @r15+,macl
        lds.l   @r15+,mach
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
.Lmaxpts:
.ifdef ROWS_TEST
        .short  12                      ! (test: WHOLE_MAX)
.else
        .short  514                     ! 2 x MAX_ROW (WHOLE_MAX)
.endif
        .short  0
.Lgrid_face:
        .long   _grid_face_asm
