! A dynamic light's share at all of a face's grid points, in SH-2 assembly
! (src/render.c dl_face's inner loops, which the compiler kept spilling):
!
!   void dl_rows(u32 *acc, dl_pts *q)
!
! Row by row, the face's (nv + 1) x (nu + 1) points into acc[], one after
! another. A row's first point is q's p (the row's start less the light,
! 16.16), then e0 on, d on each to the last but one, e1 on to the last (the
! grid's steps: grid_step's); the next row's start is f0 on (after the
! first), dv (between), f1 (before the last). At each point, d2 = dx^2 + dy^2
! + dz^2 of the whole units (>> 16: swap.w and exts.w give just that); in
! reach (d2 < r2) its weight f = ((r2 - d2) inv) >> 8 and the colours
! (c f) >> 16 packed r, g << 10, b << 20 are added to acc[i], as the C did,
! bit for bit. q's p and j are its own (it steps them).

        .text
        .align  2
        .global _dl_rows

Q_R2    = 0                             ! (dl_pts: src/render.c)
Q_INV   = 4
Q_LR    = 8
Q_LG    = 12
Q_LB    = 16
Q_P     = 20
Q_E0    = 32
Q_D     = 44
Q_E1    = 56
R_F0    = 4                             ! (the rest from q + 64)
R_NU    = 40
R_NV    = 44
R_J     = 48

! the point at r8 r9 r10 into acc[] at r4, which moves on. Uses r1-r3, MACL
.macro  POINT
        swap.w  r8,r1
        exts.w  r1,r1
        muls.w  r1,r1
        swap.w  r9,r2
        exts.w  r2,r2
        sts     macl,r3
        muls.w  r2,r2
        swap.w  r10,r1
        exts.w  r1,r1
        sts     macl,r2
        muls.w  r1,r1
        add     r2,r3
        sts     macl,r2
        add     r2,r3                   ! d2
        cmp/gt  r3,r11                  ! in reach?
        bf      91f
        bsr     .Llit
        nop
91:     add     #4,r4
.endm

_dl_rows:                               ! r4 acc, r5 q
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        sts.l   pr,@-r15
        mov     r5,r6
        mov.l   @(Q_R2,r6),r11
        mov.l   @(Q_INV,r6),r12
        mov.l   @(Q_D,r6),r7            ! d: r7 r13 r14
        mov.l   @(Q_D+4,r6),r13
        mov.l   @(Q_D+8,r6),r14
        mov.l   @(Q_P,r6),r8
        mov.l   @(Q_P+4,r6),r9
        mov.l   @(Q_P+8,r6),r10
.Lrow:                                  ! r8 r9 r10: the row's start
        mov     r6,r1
        add     #64,r1
        mov.l   @(R_NU,r1),r5
        POINT                           ! the first
        mov.l   @(Q_E0,r6),r1
        add     r1,r8
        mov.l   @(Q_E0+4,r6),r1
        add     r1,r9
        mov.l   @(Q_E0+8,r6),r1
        add     r1,r10
        POINT                           ! the second
        add     #-2,r5                  ! those between: 2 .. nu - 1
        cmp/pl  r5
        bf      2f
1:      add     r7,r8
        add     r13,r9
        add     r14,r10
        POINT
        dt      r5
        bf      1b
        bra     3f
        nop
2:      cmp/pz  r5                      ! nu 1: done; nu 2: the last
        bf      .Lnext
3:      mov.l   @(Q_E1,r6),r1           ! the last: e1 on
        add     r1,r8
        mov.l   @(Q_E1+4,r6),r1
        add     r1,r9
        mov     r6,r1
        add     #Q_E1+8,r1
        mov.l   @r1,r1
        add     r1,r10
        POINT
.Lnext:                                 ! the next row: its start f0, dv or f1 on
        mov     r6,r3
        add     #64,r3
        mov.l   @(R_J,r3),r0            ! the row just done
        mov.l   @(R_NV,r3),r1
        cmp/eq  r1,r0
        bt      .Ldone
        add     #1,r0
        mov.l   r0,@(R_J,r3)
        mov     r3,r2
        cmp/eq  #1,r0
        bt/s    .Lstep                  ! row 1: f0
        add     #R_F0,r2
        cmp/eq  r1,r0
        bt/s    .Lstep                  ! the last: f1
        add     #24,r2
        add     #-12,r2                 ! between: dv
.Lstep:
        mov.l   @(Q_P,r6),r8
        mov.l   @r2,r1
        add     r1,r8
        mov.l   r8,@(Q_P,r6)
        mov.l   @(Q_P+4,r6),r9
        mov.l   @(4,r2),r1
        add     r1,r9
        mov.l   r9,@(Q_P+4,r6)
        mov.l   @(Q_P+8,r6),r10
        mov.l   @(8,r2),r1
        add     r1,r10
        bra     .Lrow
        mov.l   r10,@(Q_P+8,r6)
.Ldone:
        lds.l   @r15+,pr
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

! in reach (d2 in r3): the weight and the colours into acc[] at r4. Uses
! r1-r3, MACL
.Llit:
        mov     r11,r1
        sub     r3,r1                   ! r2 - d2
        mul.l   r12,r1
        mov.l   @(Q_LR,r6),r2
        sts     macl,r1
        shlr8   r1                      ! f
        mul.l   r1,r2
        mov.l   @(Q_LG,r6),r3
        sts     macl,r2
        mul.l   r1,r3
        shlr16  r2                      ! r
        sts     macl,r3
        shlr16  r3
        shll8   r3
        shll2   r3                      ! g << 10
        or      r3,r2
        mov.l   @(Q_LB,r6),r3
        mul.l   r1,r3
        sts     macl,r3
        shlr16  r3
        shll16  r3
        shll2   r3
        shll2   r3                      ! b << 20
        or      r3,r2
        mov.l   @r4,r3
        add     r3,r2
        rts
        mov.l   r2,@r4
