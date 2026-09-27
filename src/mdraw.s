! draw_model's inner loops in SH-2 assembly.
!
! mverts_asm: a model's vertices into screen space (the pass over the whole
! mesh; the far mesh's subset stays in C): for each vertex its depth, and if
! it's in front of the near plane its screen position (clamped as
! screen_xy does) and outcode; the nearest and farthest depths among those.
!
!   void mverts_asm(mverts_args *a)
!
! The DSP's results come in blocks of 16 vertices: 16 x, then 16 y, then 16
! z. The divide (FOCAL << 32 / z, through GBR) runs while the depth is
! stored and compared. Everything stays in registers: the compiler's
! version spilled to the stack, and with the cache writing through, every
! spill is a bus write (with the other CPU and the DSP on the bus too).

        .text
        .align  2
        .global _mverts_asm
        .global _mpolys_asm

A_OUT   = 0                             ! the DSP's results for the model
A_MXY   = 4                             ! screen positions (x << 16 | y)
A_MZ    = 8                             ! depths
A_MOC   = 12                            ! outcodes
A_N     = 16                            ! vertices
A_ZMIN  = 20                            ! in and out
A_ZMAX  = 24

OC_NEAR = 16

_mverts_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        stc.l   gbr,@-r15
        mov.l   r4,@-r15
        mov.l   @(A_OUT,r4),r6          ! this vertex's x
        mov.l   @(A_MXY,r4),r8
        mov.l   @(A_MZ,r4),r9
        mov.l   @(A_MOC,r4),r10
        mov.l   @(A_N,r4),r5
        mov.l   @(A_ZMIN,r4),r11
        mov.l   @(A_ZMAX,r4),r12
        mov     #16,r7                  ! vertices left in the block
        mov.l   .Ldiv,r0
        ldc     r0,gbr                  ! the divider
        tst     r5,r5
        bt      .Ldone
.Lloop:
        mov.l   @r6,r13                 ! x
        mov     r6,r1
        add     #64,r1
        mov.l   @r1,r14                 ! y
        add     #64,r1
        mov.l   @r1,r3                  ! z
        mov.l   .Lnear,r1
        mov.l   r3,@r9                  ! its depth
        cmp/ge  r1,r3
        bf      .Lnear_v                ! behind the near plane
        mov     r3,r0
        mov.l   r0,@(0,gbr)             ! DVSR: z
        mov.w   .Lfocal,r0
        mov.l   r0,@(16,gbr)            ! DVDNTH: FOCAL
        mov     #0,r0
        mov.l   r0,@(20,gbr)            ! DVDNTL: 0, and it starts
        cmp/gt  r3,r11                  ! meanwhile the depths
        bf      1f
        mov     r3,r11
1:      cmp/gt  r12,r3
        bf      2f
        mov     r3,r12
2:      mov.l   @(20,gbr),r0            ! FOCAL / z, 16.16 (waits if need be)
        dmuls.l r0,r13
        mov.w   .Lclamp,r2
        sts     mach,r1
        dmuls.l r0,r14
        mov.w   .Lcx,r0
        add     r0,r1                   ! sx = CX + x r
        neg     r2,r3
        cmp/gt  r2,r1                   ! clamped to +-CLAMP_XY
        bf      3f
        mov     r2,r1
3:      cmp/ge  r3,r1
        bt      4f
        mov     r3,r1
4:      sts     mach,r0
        neg     r0,r0
        add     #112,r0                 ! sy = CY - y r
        cmp/gt  r2,r0
        bf      5f
        mov     r2,r0
5:      cmp/ge  r3,r0
        bt      6f
        mov     r3,r0
6:      extu.w  r0,r13
        mov     r1,r14
        shll16  r14
        or      r13,r14
        mov.l   r14,@r8                 ! x << 16 | y
        mov     #0,r13                  ! the outcode: left 1, right 2, top 4, bottom 8
        cmp/pz  r1
        bt      7f
        bra     8f
        mov     #1,r13
7:      mov.w   .Lw,r2
        cmp/ge  r2,r1
        bf      8f
        mov     #2,r13
8:      cmp/pz  r0
        bt      9f
        bra     .Loc
        add     #4,r13
9:      mov.w   .Lh,r2
        cmp/ge  r2,r0
        bf      .Loc
        add     #8,r13
.Loc:   mov.b   r13,@r10
.Lnext:
        add     #4,r8
        add     #4,r9
        add     #1,r10
        dt      r7
        bf.s    10f
        add     #4,r6
        mov     #16,r7                  ! the next block: past this one's y and z
        add     #127,r6
        add     #1,r6
10:     dt      r5
        bf      .Lloop
.Ldone:
        mov.l   @r15+,r4
        mov.l   r11,@(A_ZMIN,r4)
        mov.l   r12,@(A_ZMAX,r4)
        ldc.l   @r15+,gbr
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

.Lnear_v:
        mov     #OC_NEAR,r0
        bra     .Lnext
        mov.b   r0,@r10

        .align  1
.Lfocal:
        .short  160                     ! FOCAL
.Lclamp:
        .short  2000                    ! CLAMP_XY
.Lcx:
        .short  160                     ! CX
.Lw:
        .short  320                     ! SCREEN_W
.Lh:
        .short  224                     ! SCREEN_H
        .align  2
.Ldiv:
        .long   0xFFFFFF00
.Lnear:
        .long   0x80000                 ! NEAR_Z: 8.0

! mpolys_asm: the polygons facing the camera, into depth buckets (draw_model's
! second pass, which it replaces, polygon for polygon):
!
!   void mpolys_asm(mpolys_args *a)
!
! Dropped: all four corners off one side of the screen, any behind the near
! plane, or the corners going the wrong way round (the cross product of the
! first two edges; if that's 0 and it's a quad, of the next two). Kept: into
! bucket (depth - nearest) * inv (0 to 31) by the average of corners 0 and
! 2, pushed onto that bucket's list (mnext is the 64 bytes after mhead).
! Screen coordinates fit 16 bits, so the products are MULS.W.

P_POLYS = 0                             ! q_mpoly: v[4], tex, flags (12 bytes)
P_N     = 4
P_MOC   = 8
P_MXY   = 12
P_MZ    = 16
P_HEAD  = 20                            ! mhead[32], then mnext[]
P_ZMIN  = 24
P_INV   = 28

_mpolys_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        mov.l   @(P_POLYS,r4),r14
        mov.l   @(P_N,r4),r13
        mov.l   @(P_MOC,r4),r7
        mov.l   @(P_MXY,r4),r8
        mov.l   @(P_MZ,r4),r9
        mov.l   @(P_HEAD,r4),r10
        mov.l   @(P_ZMIN,r4),r12
        mov.l   @(P_INV,r4),r6
        mov     #0,r5                   ! the polygon's number
        tst     r13,r13
        bt      .Lpdone
.Lploop:
        ! the corners' outcodes: all off one side, or any behind the near plane
        mov.w   @r14,r0
        extu.w  r0,r0
        mov.b   @(r0,r7),r1
        mov.w   @(2,r14),r0
        extu.w  r0,r0
        mov.b   @(r0,r7),r2
        mov.w   @(4,r14),r0
        extu.w  r0,r0
        mov.b   @(r0,r7),r3
        mov.w   @(6,r14),r0
        extu.w  r0,r0
        mov.b   @(r0,r7),r0
        mov     r1,r4
        and     r2,r4
        and     r3,r4
        and     r0,r4
        tst     r4,r4
        bf      .Lpskip
        or      r2,r1
        or      r3,r1
        or      r1,r0
        tst     #OC_NEAR,r0
        bf      .Lpskip
        ! which way round: (b - a) x (c - a)
        mov.w   @r14,r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r8),r1             ! a
        mov.w   @(2,r14),r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r8),r2             ! b
        mov.w   @(4,r14),r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r8),r3             ! c
        swap.w  r1,r4
        exts.w  r4,r4                   ! xa
        exts.w  r1,r1                   ! ya
        swap.w  r2,r0
        exts.w  r0,r0
        sub     r4,r0                   ! xb - xa
        exts.w  r2,r2
        sub     r1,r2                   ! yb - ya
        swap.w  r3,r11
        exts.w  r11,r11
        sub     r4,r11                  ! xc - xa
        exts.w  r3,r3
        sub     r1,r3                   ! yc - ya
        muls.w  r0,r3
        sts     macl,r1
        muls.w  r2,r11
        sts     macl,r2
        sub     r2,r1                   ! the cross product
        tst     r1,r1
        bt      .Lpflat                 ! 0: a quad's other half decides
.Lpside:
        cmp/pz  r1
        bt      .Lpskip                 ! facing away (MODEL_FRONT -1)
        ! its bucket: ((corner 0's depth / 2 + corner 2's / 2 - nearest) >> 16) * inv >> 16, 0 to 31
        mov.w   @r14,r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r9),r1
        mov.w   @(4,r14),r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r9),r2
        shar    r1
        shar    r2
        add     r2,r1
        sub     r12,r1
        shlr16  r1
        exts.w  r1,r1                   ! (>> 16, arithmetic)
        mul.l   r6,r1
        sts     macl,r1
        shlr16  r1
        exts.w  r1,r1
        cmp/pz  r1
        bt      11f
        mov     #0,r1
11:     mov     #31,r0
        cmp/gt  r0,r1
        bf      12f
        mov     r0,r1
12:     mov     r1,r0
        add     r0,r0
        mov.w   @(r0,r10),r1            ! the bucket's list so far
        mov.w   r5,@(r0,r10)            ! ... now starts with this one
        mov     r5,r0
        add     r0,r0
        add     #64,r0
        mov.w   r1,@(r0,r10)            ! and goes on with it
.Lpskip:
        add     #12,r14
        dt      r13
        bf.s    .Lploop
        add     #1,r5
.Lpdone:
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

.Lpflat:
        ! (c - a) x (d - a), unless it's a triangle (flags bit 0)
        mov.w   @(10,r14),r0
        tst     #1,r0
        bf      .Lpside
        mov.w   @r14,r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r8),r1             ! a
        mov.w   @(6,r14),r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r8),r2             ! d
        swap.w  r1,r4
        exts.w  r4,r4                   ! xa
        exts.w  r1,r1                   ! ya
        swap.w  r2,r0
        exts.w  r0,r0
        sub     r4,r0                   ! xd - xa
        exts.w  r2,r2
        sub     r1,r2                   ! yd - ya
        muls.w  r11,r2                  ! (xc - xa)(yd - ya)
        sts     macl,r1
        muls.w  r3,r0                   ! (yc - ya)(xd - xa)
        sts     macl,r2
        bra     .Lpside
        sub     r2,r1
