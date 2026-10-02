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
! z. A vertex's divide (FOCAL << 32 / z, through GBR) is started as soon as
! the one before has its quotient, and runs while that one's screen position
! and outcode are worked out. Everything stays in registers: the compiler's
! version spilled to the stack, and with the cache writing through, every
! spill is a bus write (with the other CPU and the DSP on the bus too).

        .text
        .align  2
        .global _mverts_asm
        .global _mpolys_asm
        .global _mcmds_asm
        .global _vverts_asm

A_OUT   = 0                             ! the DSP's results for the model
A_MXY   = 4                             ! screen positions (x << 16 | y)
A_MZ    = 8                             ! depths
A_MOC   = 12                            ! outcodes
A_N     = 16                            ! vertices
A_ZMIN  = 20                            ! in and out
A_ZMAX  = 24

OC_NEAR = 16

! start FOCAL << 32 / \reg on the divider (at GBR). Uses r0
.macro  DIVSTART reg
        mov     \reg,r0
        mov.l   r0,@(0,gbr)             ! DVSR
        mov.w   .Lfocal,r0
        mov.l   r0,@(16,gbr)            ! DVDNTH
        mov     #0,r0
        mov.l   r0,@(20,gbr)            ! DVDNTL: starts it
.endm

! on to the next vertex (past the block's y and z after its 16th); if
! there is one, its z into r3 and, in front of the near plane, its divide
! started. Uses r0 r1
.macro  ADVANCE
        add     #4,r6
        dt      r7
        bf      31f
        mov     #16,r7
        add     #127,r6
        add     #1,r6
31:     mov     #1,r0
        cmp/gt  r0,r5
        bf      32f                     ! that was the last
        mov     r6,r1
        add     #127,r1
        add     #1,r1
        mov.l   @r1,r3                  ! its z
        mov.l   .Lnear,r1
        cmp/ge  r1,r3
        bf      32f
        DIVSTART r3
32:
.endm

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
        mov.l   @(A_N,r4),r5            ! vertices left, this one included
        mov.l   @(A_ZMIN,r4),r11
        mov.l   @(A_ZMAX,r4),r12
        mov     #16,r7                  ! vertices left in the block
        mov.l   .Ldiv,r0
        ldc     r0,gbr                  ! the divider
        tst     r5,r5
        bf      0f
        bra     .Ldone
        nop
0:      mov.l   @r6,r13                 ! the first vertex: x y z, its divide started
        mov     r6,r1
        add     #64,r1
        mov.l   @r1,r14
        add     #64,r1
        mov.l   @r1,r3
        mov.l   .Lnear,r1
        cmp/ge  r1,r3
        bf      .Lloop
        DIVSTART r3
.Lloop:
        ! x r13, y r14, z r3; its divide running if it's in front of the near plane
        mov.l   r3,@r9                  ! its depth
        mov.l   .Lnear,r1
        cmp/ge  r1,r3
        bf      .Lnear_v
        cmp/gt  r3,r11                  ! the depths
        bf      1f
        mov     r3,r11
1:      cmp/gt  r12,r3
        bf      2f
        mov     r3,r12
2:      mov.l   @(20,gbr),r0            ! FOCAL / z, 16.16
        mov     r0,r4
        ADVANCE                         ! the next one's divide starts now
        dmuls.l r4,r13
        mov.w   .Lclamp,r2
        sts     mach,r1
        dmuls.l r4,r14
        mov.w   .Lcx,r0
        add     r0,r1                   ! sx = CX + x r
        neg     r2,r4
        cmp/gt  r2,r1                   ! clamped to +-CLAMP_XY
        bf      3f
        mov     r2,r1
3:      cmp/ge  r4,r1
        bt      4f
        mov     r4,r1
4:      sts     mach,r0
        neg     r0,r0
        add     #112,r0                 ! sy = CY - y r
        cmp/gt  r2,r0
        bf      5f
        mov     r2,r0
5:      cmp/ge  r4,r0
        bt      6f
        mov     r4,r0
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
        ! the next one's x and y (its z's in r3 already)
        mov.l   @r6,r13
        mov     r6,r1
        add     #64,r1
        mov.l   @r1,r14
        add     #4,r8
        add     #4,r9
        dt      r5
        bf.s    .Lloop
        add     #1,r10
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
        mov.b   r0,@r10
        ADVANCE
        bra     .Lnext
        nop

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
!
! Every model (and the gun in your hands) passes P_SIGN -1: the cross product
! negated (kept in GBR), as front faces wind (MODEL_FRONT 1). The gun's
! polygons across the near plane aren't dropped but listed (P_NEAR), for the C
! to cut at the plane.

P_POLYS = 0                             ! q_mpoly: v[4], tex, flags (12 bytes)
P_N     = 4
P_MOC   = 8
P_MXY   = 12
P_MZ    = 16
P_HEAD  = 20                            ! mhead[32], then mnext[]
P_ZMIN  = 24
P_INV   = 28
P_SIGN  = 32                            ! -1: MODEL_FRONT 1 (every model); 0: the other way round
P_NEAR  = 36                            ! s16 list of those across the near plane (0: dropped)
P_NNEAR = 40                            ! ...how many (in and out)

_mpolys_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        stc.l   gbr,@-r15
        mov.l   r4,@-r15                ! (the arguments, for .Lpnear)
        mov.l   @(P_SIGN,r4),r0
        ldc     r0,gbr
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
        bf      .Lpnear
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
        stc     gbr,r0
        xor     r0,r1
        sub     r0,r1                   ! (negated for the gun)
        cmp/pz  r1
        bt      .Lpskip                 ! facing away
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
        add     #4,r15                  ! (the arguments)
        ldc.l   @r15+,gbr
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

.Lpnear:
        ! across the near plane: listed if there's a list, else dropped
        mov.l   @r15,r1
        mov.l   @(P_NEAR,r1),r2
        tst     r2,r2
        bt      .Lpskip
        mov.l   @(P_NNEAR,r1),r0
        mov     r0,r3
        add     #1,r3
        mov.l   r3,@(P_NNEAR,r1)
        add     r0,r0
        bra     .Lpskip
        mov.w   r5,@(r0,r2)

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

! mcmds_asm: the polygons kept, bucket by bucket, into VDP1 commands in this
! CPU's list (draw_model's last pass, which it replaces, command for command):
!
!   void mcmds_asm(mcmds_args *a)
!
! A textured, Gouraud-shaded distorted sprite a polygon: its texture's slot
! in the VRAM cache (tex_load, in C, if it's not there; skipped if there's no
! room), linked into this CPU's current bucket the way cmd_alloc does it,
! its Gouraud table after the others. What's read for every polygon is in
! the first 16 words (@(disp,Rn) reaches 60 bytes); the rest is by R0. The
! list's count, the bucket's ends and the Gouraud count stay in registers.

M_POLYS  = 0
M_TEX    = 4                            ! q_mtex: ofs, w, h (8 bytes)
M_NEXT   = 8                            ! mnext
M_MXY    = 12
M_MG     = 16
M_TSLOT  = 20                           ! texture -> slot (0xFFFF: none)
M_SFRAME = 24                           ! slot -> the frame it was last used
M_CMDS   = 28                           ! this CPU's commands (32 bytes)
M_GST    = 32                           ! its Gouraud tables (8 bytes)
M_TID0   = 36                           ! the model's first texture id, this skin's
M_FRAME  = 40
M_SVRAM  = 44                           ! the slots' VRAM
M_SBYTES = 48                           ! a slot's size
M_LB     = 52                           ! the list's LINK for command 0
M_DW1    = 56                           ! the command's second word, for colour table 0
M_SLUT   = 60                           ! slot -> its colour table (u16s: tex_load puts them)
M_GMAX   = 64
M_GB     = 68                           ! the Gouraud tables' VRAM / 8
M_FIFO   = 72                           ! appended (in order) rather than pushed
M_X      = 76                           ! the r_ctx, for tex_load
M_CNT    = 80                           ! in and out
M_HEAD   = 84
M_TAIL   = 88
M_GC     = 92
M_DROP   = 96                           ! out: none left in the list
M_HEADS  = 100                          ! mhead
M_WCMDS  = 104                          ! the writer's room (its cmax)

_mcmds_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        sts.l   pr,@-r15
        mov     r4,r14
        mov     #M_CNT,r0
        mov.l   @(r0,r14),r12           ! the list's count
        mov     #M_HEAD,r0
        mov.l   @(r0,r14),r11           ! the bucket's first
        mov     #M_TAIL,r0
        mov.l   @(r0,r14),r10           ! ... and last
        mov     #M_GC,r0
        mov.l   @(r0,r14),r9            ! Gouraud tables used
        mov     #0,r8                   ! the depth bucket, 0 to 31 (far to near appended, near to far pushed)
.Lcbucket:
        mov     #M_HEADS,r0
        mov.l   @(r0,r14),r1
        mov     #M_FIFO,r0
        mov.l   @(r0,r14),r0
        tst     r0,r0
        bt.s    1f
        mov     r8,r0
        mov     #31,r0
        sub     r8,r0
1:      add     r0,r0
        mov.w   @(r0,r1),r13            ! the bucket's first polygon
.Lcpoly:
        cmp/pz  r13
        bt      2f
        bra     .Lcnextb
        nop
2:      mov     r13,r0                  ! the polygon: polys + i * 12
        shll2   r0
        mov     r0,r1
        add     r0,r0
        add     r1,r0
        mov.l   @(M_POLYS,r14),r5
        add     r0,r5
        mov.w   @(8,r5),r0
        extu.w  r0,r6                   ! its texture
        mov.l   @(M_TID0,r14),r1
        add     r6,r1                   ! ... its id
        mov     r1,r0
        add     r0,r0
        mov.l   @(M_TSLOT,r14),r2
        mov.w   @(r0,r2),r0
        cmp/eq  #-1,r0
        bf      13f
        bra     .Lcmiss                 ! not in the cache
        nop
13:     extu.w  r0,r2                   ! the slot
        mov.l   @(M_SFRAME,r14),r1
        add     r0,r0
        mov.l   @(M_FRAME,r14),r3
        mov.w   r3,@(r0,r1)             ! used this frame
        mov.l   @(M_SBYTES,r14),r3
        mulu.w  r2,r3
        mov.l   @(M_SVRAM,r14),r1
        sts     macl,r3
        add     r1,r3                   ! its VRAM
.Lchave:
        mov.l   @(M_SLUT,r14),r7
        mov     r2,r0
        add     r0,r0
        mov.w   @(r0,r7),r7
        extu.w  r7,r7
        shll2   r7
        mov.l   @(M_DW1,r14),r0
        add     r0,r7                   ! PMOD, COLR: the slot's colour table
        mov     #M_WCMDS,r0
        mov.l   @(r0,r14),r0
        cmp/ge  r0,r12
        bf      14f
        bra     .Lcdrop                 ! the list's full
        nop
14:     mov     #0,r4                   ! the new command's LINK
        mov     #M_FIFO,r0
        mov.l   @(r0,r14),r0
        tst     r0,r0
        bt      .Lcpush
        ! appended: the last one links to it
        cmp/pz  r11
        bt      3f
        bra     4f
        mov     r12,r11                 ! (the first in the bucket)
3:      mov     r10,r0
        shll2   r0
        shll2   r0
        shll    r0
        add     #2,r0                   ! the last one's LINK
        mov.l   @(M_LB,r14),r1
        mov     r12,r2
        shll2   r2
        add     r2,r1
        mov.l   @(M_CMDS,r14),r2
        mov.w   r1,@(r0,r2)
4:      bra     .Lcemit
        mov     r12,r10
.Lcpush:
        ! pushed: it links to the first
        cmp/pz  r11
        bt      5f
        bra     6f
        mov     r12,r10                 ! (the first in the bucket: also its last)
5:      mov.l   @(M_LB,r14),r4
        mov     r11,r0
        shll2   r0
        add     r0,r4
6:      mov     r12,r11
.Lcemit:
        mov     r12,r0
        shll2   r0
        shll2   r0
        shll    r0
        mov.l   @(M_CMDS,r14),r2
        add     r0,r2                   ! the command
        add     #1,r12
        mov.l   .Lctrl,r0
        or      r4,r0
        mov.l   r0,@r2                  ! jump-assign, distorted sprite; LINK
        mov.l   r7,@(4,r2)              ! PMOD, COLR
        mov.l   @(M_TEX,r14),r1
        mov     r6,r0
        shll2   r0
        add     r0,r0
        add     r0,r1                   ! its texture's record
        add     #4,r1
        mov.b   @r1+,r4                 ! w
        mov.b   @r1,r1                  ! h
        extu.b  r4,r4
        extu.b  r1,r1
        shlr2   r4
        shlr    r4
        shll8   r4
        or      r1,r4                   ! SIZE: w / 8, h
        mov     r3,r0
        shlr2   r0
        shlr    r0
        shll16  r0
        or      r4,r0
        mov.l   r0,@(8,r2)              ! SRCA, SIZE
        mov.l   @(M_MXY,r14),r1
        mov.w   @r5,r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r1),r0
        mov.l   r0,@(12,r2)             ! A
        mov.w   @(2,r5),r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r1),r0
        mov.l   r0,@(16,r2)             ! B
        mov.w   @(4,r5),r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r1),r0
        mov.l   r0,@(20,r2)             ! C
        mov.w   @(6,r5),r0
        extu.w  r0,r0
        shll2   r0
        mov.l   @(r0,r1),r0
        mov.l   r0,@(24,r2)             ! D
        ! its Gouraud table (the last one over again if they've run out)
        mov     r9,r3
        mov     #M_GMAX,r0
        mov.l   @(r0,r14),r0
        cmp/ge  r0,r3
        bf.s    7f
        add     #1,r9
        add     #-1,r9
        add     #-1,r0
        mov     r0,r3
7:      mov.l   @(M_MG,r14),r1
        mov.w   @r5,r0
        extu.w  r0,r0
        add     r0,r0
        mov.w   @(r0,r1),r4
        shll16  r4
        mov.w   @(2,r5),r0
        extu.w  r0,r0
        add     r0,r0
        mov.w   @(r0,r1),r0
        extu.w  r0,r0
        or      r0,r4
        mov.l   @(M_GST,r14),r6
        mov     r3,r0
        shll2   r0
        add     r0,r0
        add     r0,r6
        mov.l   r4,@r6
        mov.w   @(4,r5),r0
        extu.w  r0,r0
        add     r0,r0
        mov.w   @(r0,r1),r4
        shll16  r4
        mov.w   @(6,r5),r0
        extu.w  r0,r0
        add     r0,r0
        mov.w   @(r0,r1),r0
        extu.w  r0,r0
        or      r0,r4
        mov.l   r4,@(4,r6)
        mov     #M_GB,r0
        mov.l   @(r0,r14),r0
        add     r3,r0
        shll16  r0
        mov.l   r0,@(28,r2)             ! GRDA
.Lcnext:
        mov     r13,r0
        add     r0,r0
        mov.l   @(M_NEXT,r14),r1
        bra     .Lcpoly
        mov.w   @(r0,r1),r13

.Lcnextb:
        add     #1,r8
        mov     #32,r0
        cmp/ge  r0,r8
        bt      8f
        bra     .Lcbucket
        nop
8:      mov     #M_CNT,r0
        mov.l   r12,@(r0,r14)
        mov     #M_HEAD,r0
        mov.l   r11,@(r0,r14)
        mov     #M_TAIL,r0
        mov.l   r10,@(r0,r14)
        mov     #M_GC,r0
        mov.l   r9,@(r0,r14)
        lds.l   @r15+,pr
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

.Lcmiss:
        ! the texture into the cache (C: tex_load(x, id)); none free: skip it
        mov     #M_X,r0
        mov.l   @(r0,r14),r4
        mov     r1,r5
        mov.l   .Ltexload,r0
        jsr     @r0
        nop
        cmp/pz  r0
        bf      .Lcnext
        mov     r0,r3                   ! its VRAM
        mov     r13,r0                  ! the polygon again (the call used r5 r6)
        shll2   r0
        mov     r0,r1
        add     r0,r0
        add     r1,r0
        mov.l   @(M_POLYS,r14),r5
        add     r0,r5
        mov.w   @(8,r5),r0
        extu.w  r0,r6
        mov.l   @(M_TID0,r14),r1
        add     r6,r1
        add     r1,r1
        mov.l   @(M_TSLOT,r14),r2
        add     r1,r2
        mov.w   @r2,r2
        bra     .Lchave
        extu.w  r2,r2                   ! its slot (tex_load's)

.Lcdrop:
        mov     #M_DROP,r0
        mov.l   @(r0,r14),r1
        add     #1,r1
        bra     .Lcnext
        mov.l   r1,@(r0,r14)

        .align  2
.Lctrl:
        .long   0x10020000              ! jump-assign, distorted sprite
.Ltexload:
        .long   _tex_load

! vverts_asm: the gun's vertices (draw_viewmodel), as mverts_asm does a
! monster's, but from its two frames' packed bytes (x y z normal) rather than
! the DSP's results: each coordinate u + (v - u) lerp, u and v the two
! frames' (scale * byte + translation), as the C had it; view z from the x
! bytes, x from the y bytes (negated), y from the z bytes. Each vertex's
! normal byte (the nearer frame's) is listed for the C to light. The next
! vertex's z is worked out and its divide started as soon as this one has its
! quotient; the nearest and farthest depths stay in the arguments (the
! registers are all taken).
!
!   void vverts_asm(vverts_args *a)

V_SX0   = 0                             ! z, from the x bytes: frame 0's scale, translation, frame 1's
V_TX0   = 4
V_SX1   = 8
V_TX1   = 12
V_SY0   = 16                            ! x (right): minus the y bytes'
V_TY0   = 20
V_SY1   = 24
V_TY1   = 28
V_SZ0   = 32                            ! y (up): the z bytes'
V_TZ0   = 36
V_SZ1   = 40
V_TZ1   = 44
V_L     = 48                            ! the blend, 16.16
V_ZMIN  = 52                            ! in and out
V_ZMAX  = 56
V_PA    = 60                            ! frame 0's vertices
V_PB    = 64                            ! frame 1's (this and the rest by R0)
V_MXY   = 68
V_MZ    = 72
V_MOC   = 76
V_VN    = 80
V_N     = 84
V_NOFS  = 88                            ! the normal's byte from r6: 3, or frame 1's

! a coordinate: \o from byte \k of this vertex (\n: 4 for the next one's), with
! frame 0's scale and translation at \s0 \t0 and frame 1's at \s1 \t1. Uses r0 r1 r2
.macro  COORD o k s0 t0 s1 t1
        mov.b   @(\k,r6),r0
        extu.b  r0,r1
        mov.l   @(\s0,r4),r2
        mul.l   r2,r1                   ! s0 a
        mov.b   @(\k,r7),r0
        extu.b  r0,r0
        mov.l   @(\s1,r4),r2
        sts     macl,\o
        mul.l   r2,r0                   ! s1 b
        mov.l   @(\t0,r4),r2
        add     r2,\o                   ! u
        mov.l   @(\t1,r4),r2
        sts     macl,r1
        add     r2,r1                   ! v
        sub     \o,r1
        mov.l   @(V_L,r4),r2
        dmuls.l r2,r1
        sts     mach,r1
        sts     macl,r2
        xtrct   r1,r2                   ! (v - u) lerp
        add     r2,\o
.endm

! the next vertex's z into r3 (r6 r7: this one's bytes)
.macro  NEXTZ
        COORD   r3, 4, V_SX0, V_TX0, V_SX1, V_TX1
.endm

! this vertex's x and y into r13 r14
.macro  THISXY
        COORD   r13, 1, V_SY0, V_TY0, V_SY1, V_TY1
        neg     r13,r13
        COORD   r14, 2, V_SZ0, V_TZ0, V_SZ1, V_TZ1
.endm

! start FOCAL << 32 / \reg on the divider (at GBR). Uses r0
.macro  VDIVSTART reg
        mov     \reg,r0
        mov.l   r0,@(0,gbr)             ! DVSR
        mov.l   .Lvfocal,r0
        mov.l   r0,@(16,gbr)            ! DVDNTH
        mov     #0,r0
        mov.l   r0,@(20,gbr)            ! DVDNTL: starts it
.endm

! the next vertex's z into r3, its divide started if it's in front of the near plane
.macro  VADVANCE
        NEXTZ
        mov.l   .Lvnear,r1
        cmp/ge  r1,r3
        bf      33f
        VDIVSTART r3
33:
.endm

_vverts_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        stc.l   gbr,@-r15
        mov.l   @(V_PA,r4),r6
        mov     #V_PB,r0
        mov.l   @(r0,r4),r7
        mov     #V_MXY,r0
        mov.l   @(r0,r4),r8
        mov     #V_MZ,r0
        mov.l   @(r0,r4),r9
        mov     #V_MOC,r0
        mov.l   @(r0,r4),r10
        mov     #V_VN,r0
        mov.l   @(r0,r4),r11
        mov     #V_N,r0
        mov.l   @(r0,r4),r5             ! vertices left, this one included
        mov.l   .Lvdiv,r0
        ldc     r0,gbr                  ! the divider
        tst     r5,r5
        bf      0f
        bra     .Lvdone
        nop
0:      add     #-4,r6                  ! the first one's z (NEXTZ looks a vertex on), its divide
        add     #-4,r7
        VADVANCE
        add     #4,r6
        add     #4,r7
        THISXY
.Lvloop:
        ! x r13, y r14, z r3 (its divide running if it's in front of the near plane); r6 r7 its bytes
        mov     #V_NOFS,r0
        mov.l   @(r0,r4),r0
        mov.b   @(r0,r6),r1
        mov.b   r1,@r11                 ! its normal
        mov.l   r3,@r9                  ! its depth
        mov.l   .Lvnear,r1
        cmp/ge  r1,r3
        bt      1f
        bra     .Lvnear_v
        nop
1:      mov.l   @(V_ZMIN,r4),r1         ! the depths
        cmp/gt  r3,r1
        bf      2f
        mov.l   r3,@(V_ZMIN,r4)
2:      mov.l   @(V_ZMAX,r4),r1
        cmp/gt  r1,r3
        bf      3f
        mov.l   r3,@(V_ZMAX,r4)
3:      mov.l   @(20,gbr),r0            ! FOCAL / z, 16.16
        mov     r0,r12
        VADVANCE                        ! the next one's divide starts now
        dmuls.l r12,r13
        mov.l   .Lvclamp,r2
        sts     mach,r1
        dmuls.l r12,r14
        mov.l   .Lvcx,r0
        add     r0,r1                   ! sx = CX + x r
        neg     r2,r12
        cmp/gt  r2,r1                   ! clamped to +-CLAMP_XY
        bf      4f
        mov     r2,r1
4:      cmp/ge  r12,r1
        bt      5f
        mov     r12,r1
5:      sts     mach,r0
        neg     r0,r0
        add     #112,r0                 ! sy = CY - y r
        cmp/gt  r2,r0
        bf      6f
        mov     r2,r0
6:      cmp/ge  r12,r0
        bt      7f
        mov     r12,r0
7:      extu.w  r0,r13
        mov     r1,r14
        shll16  r14
        or      r13,r14
        mov.l   r14,@r8                 ! x << 16 | y
        mov     #0,r13                  ! the outcode: left 1, right 2, top 4, bottom 8
        cmp/pz  r1
        bt      8f
        bra     9f
        mov     #1,r13
8:      mov.l   .Lvw,r2
        cmp/ge  r2,r1
        bf      9f
        mov     #2,r13
9:      cmp/pz  r0
        bt      10f
        bra     .Lvoc
        add     #4,r13
10:     mov.l   .Lvh,r2
        cmp/ge  r2,r0
        bf      .Lvoc
        add     #8,r13
.Lvoc:  mov.b   r13,@r10
.Lvnext:
        ! on to the next: its x and y (its z's in r3 already)
        add     #4,r6
        add     #4,r7
        add     #4,r8
        add     #4,r9
        add     #1,r10
        dt      r5
        bt.s    .Lvdone
        add     #1,r11
        THISXY
        bra     .Lvloop
        nop

.Lvnear_v:
        mov     #OC_NEAR,r0
        mov.b   r0,@r10
        VADVANCE
        bra     .Lvnext
        nop

.Lvdone:
        ldc.l   @r15+,gbr
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

        .align  2
.Lvdiv:
        .long   0xFFFFFF00
.Lvnear:
        .long   0x40000                 ! VIEW_NEAR: 4.0
.Lvfocal:
        .long   160                     ! FOCAL
.Lvclamp:
        .long   2000                    ! CLAMP_XY
.Lvcx:
        .long   160                     ! CX
.Lvw:
        .long   320                     ! SCREEN_W
.Lvh:
        .long   224                     ! SCREEN_H

! vturn_asm: the gun's bob on n screen places (render.c view_turn: the gun turned about the
! eye; each place's direction turned by the rotation's rows and projected again), as the C
! (view_turn_c) does, each divide running while the place before is finished.
!
!   void vturn_asm(const s32 *m, const u32 *in, u32 *out, int n)
!
! m: the rows (2.14; the third column times FOCAL). A place is x << 16 | y; X right and Y up
! from the centre. One turned nearly square to the view (nz < 4096) goes to the clamp its way.

.macro  TXY                             ! r0's place -> r8 X, r9 Y
        swap.w  r0,r8
        exts.w  r8,r8
        add     #-128,r8
        add     #-32,r8                 ! X = x - CX
        exts.w  r0,r9
        neg     r9,r9
        add     #112,r9                 ! Y = CY - y
.endm

.macro  TNZ                             ! r10 = m6 X + m7 Y + m8. Uses r0 r1 r2
        mov.l   @(24,r4),r0
        mul.l   r0,r8
        mov.l   @(28,r4),r1
        sts     macl,r10
        mul.l   r1,r9
        mov.l   @(32,r4),r2
        sts     macl,r1
        add     r1,r10
        add     r2,r10
.endm

.macro  TSTART                          ! FOCAL << 32 / nz started, if nz is far enough. Uses r0
        mov.l   .Lt4096,r0
        cmp/ge  r0,r10
        bf      21f
        mov     r10,r0
        mov.l   r0,@(0,gbr)             ! DVSR
        mov.l   .Ltfocal,r0
        mov.l   r0,@(16,gbr)            ! DVDNTH
        mov     #0,r0
        mov.l   r0,@(20,gbr)            ! DVDNTL: starts it
21:
.endm

        .global _vturn_asm
_vturn_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        stc.l   gbr,@-r15
        mov.l   .Ltdiv,r0
        ldc     r0,gbr                  ! the divider
        cmp/pl  r7
        bf      .Ltdone
        mov.l   @r5,r0                  ! the first: X Y, nz, its divide
        TXY
        TNZ
        TSTART
.Ltloop:
        ! this one's nx and ny (r8 r9 its X Y, r10 its nz)
        mov.l   @(0,r4),r0
        mul.l   r0,r8
        mov.l   @(4,r4),r1
        sts     macl,r11
        mul.l   r1,r9
        mov.l   @(8,r4),r2
        sts     macl,r1
        add     r1,r11
        add     r2,r11                  ! nx
        mov.l   @(12,r4),r0
        mul.l   r0,r8
        mov.l   @(16,r4),r1
        sts     macl,r12
        mul.l   r1,r9
        mov.l   @(20,r4),r2
        sts     macl,r1
        add     r1,r12
        add     r2,r12                  ! ny
        mov     r10,r14                 ! (its nz)
        mov.l   .Lt4096,r0
        cmp/ge  r0,r14
        bf      1f
        mov.l   @(20,gbr),r0            ! its quotient
        mov     r0,r13
1:      dt      r7
        bt      2f
        mov.l   @(4,r5),r0              ! the next one's X Y, nz, divide, now
        TXY
        TNZ
        TSTART
2:      mov.l   .Lt4096,r0              ! this one onto the screen
        cmp/ge  r0,r14
        bf      .Ltfar
        dmuls.l r13,r11
        mov.l   .Ltclamp,r2
        sts     mach,r1
        dmuls.l r13,r12
        mov.l   .Ltcx,r0
        add     r0,r1                   ! sx = CX + nx r
        neg     r2,r3
        cmp/gt  r2,r1                   ! clamped to +-CLAMP_XY
        bf      3f
        mov     r2,r1
3:      cmp/ge  r3,r1
        bt      4f
        mov     r3,r1
4:      sts     mach,r0
        neg     r0,r0
        add     #112,r0                 ! sy = CY - ny r
        cmp/gt  r2,r0
        bf      5f
        mov     r2,r0
5:      cmp/ge  r3,r0
        bt      .Ltput
        mov     r3,r0
.Ltput:
        extu.w  r0,r0
        shll16  r1
        or      r1,r0
        mov.l   r0,@r6                  ! x << 16 | y
        add     #4,r6
        tst     r7,r7
        bf.s    .Ltloop
        add     #4,r5
.Ltdone:
        ldc.l   @r15+,gbr
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

.Ltfar:
        ! (turned nearly square to the view, or behind: to the clamp, its way; none, the centre)
        mov.l   .Ltclamp,r2
        mov.l   .Ltcx,r1
        cmp/pl  r11
        bf      6f
        mov     r2,r1                   ! right
6:      cmp/pz  r11
        bt      7f
        neg     r2,r1                   ! left
7:      mov     #112,r0
        cmp/pl  r12
        bf      8f
        neg     r2,r0                   ! up
8:      cmp/pz  r12
        bt      .Ltput
        bra     .Ltput
        mov     r2,r0                   ! down

        .align  2
.Ltdiv:
        .long   0xFFFFFF00
.Lt4096:
        .long   4096
.Ltfocal:
        .long   160                     ! FOCAL
.Ltclamp:
        .long   2000                    ! CLAMP_XY
.Ltcx:
        .long   160                     ! CX
