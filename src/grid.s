! A grid row, in SH-2 assembly (src/render.c's draw_face calls it for each
! row of a face's grid). For each of n points along the row: its view-space
! position, where it is on screen, and which sides of the view it's outside.
!
!   void grid_row_asm(gv *row, int n, const grid_args *a, const s32 *k)
!
!   gv:        xy (screen x << 16 | y), oc, done (bytes), pad                   8 bytes
!   grid_args: p (the row's first point), e0 (the step out of the first
!              column), d (a whole column), e1 (into the last)               12 words
!   k:         NEAR_Z, FOCAL, CX, CY, SCREEN_W, SCREEN_H, ky, CLAMP_XY, and at
!              [20] [21] the guard band (OC_FAR beyond it: 32)
!
! n >= 2. The steps go e0, then d, d, ..., then e1 (for n = 2, just e0).
!
! What makes it quicker than the compiler's:
! - The divide is pipelined. The SH-2's divider works on its own, 39 cycles a
!   divide: each point starts the next point's as soon as it has its own
!   result, so a whole point's work hides the next one's wait.
! - The outcode comes from the screen position (off the left if x < 0, and
!   so on), a compare and a ROTCL each, the T bits shifted straight into the
!   byte. The bits are in that order: NEAR 16, BOTTOM 8, TOP 4, RIGHT 2,
!   LEFT 1. A point behind the near plane has no screen position, so it
!   takes the planes instead (x < -z, and so on: kx is 1), off the main path.
! - Screen positions are clamped only for points already off the screen (on
!   that path too).
! - Everything's in registers: nothing goes on the stack in the loop.
!
! Registers: r1 r2 r3 the point, r4 the gv, r5 the count, r6 the args,
! r8 whether it's near, r11 r12 r13 the step d, r14 the constants; r0 r7
! r9 r10 as they come. GBR points at the divider (its moves are r0 only).

        .text
        .align  2
        .global _grid_row_asm
        .global _grid_face_asm

K_NEAR  = 0
K_FOCAL = 4
K_CX    = 8
K_CY    = 12
K_W     = 16
K_H     = 20
K_KY    = 24
K_LIM   = 28
K_GX    = 80                            ! the guard band, from the middle (OC_FAR beyond it)
K_GY    = 84
OC_FAR  = 32

A_E0    = 12
A_E1    = 36
A_RS    = 48                            ! (grid_face_asm) the next row's first point

K_NPTS  = 32                            ! (grid_face_asm) points a row
K_ROWS  = 36                            !   rows after this one
K_RI    = 40                            !   this row's number
K_F0    = 44                            !   the row steps: f0, dv, f1 (3 words each)

G_XY    = 0
G_OC    = 4
G_SIZE  = 8

DV_DVSR  = 0
DV_DVDNTH = 16
DV_DVDNTL = 20

! the step to the next point: from the args (e0, e1) or the registers (d)
.macro  STEPZ kind
.ifc \kind,d
        add     r13,r3
.else
.ifc \kind,row
        mov.l   @(A_RS+8,r6),r3         ! the next row's first point
.else
        mov.l   @(\kind+8,r6),r7
        add     r7,r3
.endif
.endif
.endm
.macro  STEPXY kind
.ifc \kind,d
        add     r11,r1
        add     r12,r2
.else
.ifc \kind,row
        mov.l   @(A_RS,r6),r1
        mov.l   @(A_RS+4,r6),r2
.else
        mov.l   @(\kind,r6),r7
        add     r7,r1
        mov.l   @(\kind+4,r6),r7
        add     r7,r2
.endif
.endif
.endm

! one point (its divide under way), and the next's started unless kind is "last"
.macro  POINT kind
        mov.l   @(K_NEAR,r14),r8
        cmp/gt  r3,r8                   ! z < NEAR_Z
        bt      90f                     ! behind the near plane: its outcode now, while x y z are here
        mov     #0,r8
89:
        mov.l   @(DV_DVDNTL,gbr),r0     ! FOCAL / z, 16.16 (waits for the divider)
        dmuls.l r0,r1                   ! x times it
.ifnc \kind,last
        STEPZ   \kind
.endif
        mov.l   @(K_CX,r14),r9
        sts     mach,r7                 ! screen x less CX
        dmuls.l r0,r2                   ! y times it
        add     r7,r9                   ! screen x
.ifnc \kind,last
        mov     r3,r0                   ! the next point's divide
        mov.l   r0,@(DV_DVSR,gbr)
        mov.l   @(K_FOCAL,r14),r0
        mov.l   r0,@(DV_DVDNTH,gbr)
        mov     #0,r0
        mov.l   r0,@(DV_DVDNTL,gbr)     ! go
        STEPXY  \kind
.endif
        mov.l   @(K_CY,r14),r10
        mov.l   @(K_H,r14),r7
        sts     mach,r0
        sub     r0,r10                  ! screen y
        tst     r8,r8
        bf      91f                     ! behind the near plane: its outcode's in r8
        ! the outcode: NEAR (not), then BOTTOM, TOP, RIGHT, LEFT, rotated in
        mov     #0,r0
        cmp/gt  r7,r10                  ! below: y > H
        rotcl   r0
        mov     r10,r7
        shll    r7                      ! above: y < 0
        rotcl   r0
        mov.l   @(K_W,r14),r7
        cmp/gt  r7,r9                   ! right: x > W
        rotcl   r0
        mov     r9,r7
        shll    r7                      ! left: x < 0
        rotcl   r0
        tst     r0,r0
        bf      93f                     ! off the screen
92:
        shll16  r9
        extu.w  r10,r10
        or      r10,r9
        mov.l   r9,@(G_XY,r4)
        shll8   r0
        add     #1,r0                   ! done: it's been projected
        mov.w   r0,@(G_OC,r4)           ! oc and done in one store
        bra     99f
        add     #G_SIZE,r4
91:
        bra     92b                     ! (its screen position's meaningless: nothing asks for it)
        mov     r8,r0
90:
        ! behind the near plane: the outcode from the planes (x = +-z, y = +-ky z), into r8
        mov.l   @(K_KY,r14),r7
        dmuls.l r7,r3
        sts     mach,r7
        sts     macl,r10
        xtrct   r7,r10                  ! ty
        mov     #1,r8                   ! NEAR
        mov     r2,r7
        add     r10,r7
        shll    r7                      ! below: y < -ty
        rotcl   r8
        cmp/gt  r10,r2                  ! above: y > ty
        rotcl   r8
        cmp/gt  r3,r1                   ! right: x > z
        rotcl   r8
        mov     r1,r7
        add     r3,r7
        shll    r7                      ! left: x < -z
        bra     89b
        rotcl   r8
93:
        ! far beyond the screen (the guard band, from the middle): OC_FAR, for the cells
        ! there to be split (src/render.c cell_split)
        mov.l   @(K_CX,r14),r7
        mov     r9,r8
        sub     r7,r8
        mov     r14,r7
        add     #K_GX,r7
        mov.l   @r7,r7
        cmp/gt  r7,r8
        bt      94f
        neg     r7,r7
        cmp/gt  r8,r7
        bt      94f
        mov.l   @(K_CY,r14),r7
        mov     r10,r8
        sub     r7,r8
        mov     r14,r7
        add     #K_GY,r7
        mov.l   @r7,r7
        cmp/gt  r7,r8
        bt      94f
        neg     r7,r7
        cmp/gt  r8,r7
        bf      95f
94:     or      #OC_FAR,r0
95:
        ! off the screen: keep the position within CLAMP_XY of the middle
        mov.l   @(K_LIM,r14),r7
        cmp/gt  r7,r9
        bf      1f
        mov     r7,r9
1:      cmp/gt  r7,r10
        bf      2f
        mov     r7,r10
2:      neg     r7,r7
        cmp/gt  r9,r7
        bf      3f
        mov     r7,r9
3:      cmp/gt  r10,r7
        bf      92b
        bra     92b
        mov     r7,r10
99:
.endm

_grid_row_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        sts.l   mach,@-r15
        sts.l   macl,@-r15
        stc.l   gbr,@-r15
        mov     r7,r14
        mov     #-1,r0
        shll8   r0
        ldc     r0,gbr                  ! 0xFFFFFF00: the divider, GBR-relative (r0 only)
        mov.l   @r6,r1
        mov.l   @(4,r6),r2
        mov.l   @(8,r6),r3
        mov.l   @(24,r6),r11
        mov.l   @(28,r6),r12
        mov.l   @(32,r6),r13
        mov     r3,r0                   ! the first point's divide
        mov.l   r0,@(DV_DVSR,gbr)
        mov.l   @(K_FOCAL,r14),r0
        mov.l   r0,@(DV_DVDNTH,gbr)
        mov     #0,r0
        mov.l   r0,@(DV_DVDNTL,gbr)
        POINT   A_E0
        add     #-3,r5                  ! the whole columns between
        cmp/pz  r5
        bt      1f
        bra     .Llast                  ! n = 2: e0 went all the way (too far for BF)
        nop
1:
        tst     r5,r5
        bt      .Lpenult
.Lloop:
        POINT   d
        dt      r5
        bf      .Lloop
.Lpenult:
        POINT   A_E1
.Llast:
        POINT   last
        ldc.l   @r15+,gbr
        lds.l   @r15+,macl
        lds.l   @r15+,mach
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

! A whole face's grid, row after row into one buffer (nv + 1 rows of npts):
!
!   void grid_face_asm(gv *grid, int npts, grid_args *a, s32 *k)
!
! a: as for a row, p the face's first point, and a scratch 3 words (A_RS).
! k: the constants, then npts, the rows after the first, 0, and the row
! steps f0, dv, f1 (as the columns'). The divide carries on from one row
! into the next: no row starts cold.
_grid_face_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        sts.l   mach,@-r15
        sts.l   macl,@-r15
        stc.l   gbr,@-r15
        mov     r7,r14
        mov.l   r5,@(K_NPTS,r14)
        mov     #0,r0
        mov.l   r0,@(K_RI,r14)
        mov     #-1,r0
        shll8   r0
        ldc     r0,gbr
        mov.l   @r6,r1
        mov.l   @(4,r6),r2
        mov.l   @(8,r6),r3
        mov.l   r1,@(A_RS,r6)
        mov.l   r2,@(A_RS+4,r6)
        mov.l   r3,@(A_RS+8,r6)
        mov.l   @(24,r6),r11
        mov.l   @(28,r6),r12
        mov.l   @(32,r6),r13
        mov     r3,r0                   ! the first point's divide
        mov.l   r0,@(DV_DVSR,gbr)
        mov.l   @(K_FOCAL,r14),r0
        mov.l   r0,@(DV_DVDNTH,gbr)
        mov     #0,r0
        mov.l   r0,@(DV_DVDNTL,gbr)
.Lfrow:
        ! the next row's first point: f0 after the first row, f1 before the last, dv between
        mov.l   @(K_ROWS,r14),r0
        tst     r0,r0
        bt      5f
        mov     r14,r7
        add     #K_F0,r7
        mov.l   @(K_RI,r14),r0
        tst     r0,r0
        bt      4f
        add     #12,r7
        mov.l   @(K_ROWS,r14),r0
        cmp/eq  #1,r0
        bf      4f
        add     #12,r7
4:      mov.l   @(A_RS,r6),r0
        mov.l   @r7,r9
        add     r9,r0
        mov.l   r0,@(A_RS,r6)
        mov.l   @(A_RS+4,r6),r0
        mov.l   @(4,r7),r9
        add     r9,r0
        mov.l   r0,@(A_RS+4,r6)
        mov.l   @(A_RS+8,r6),r0
        mov.l   @(8,r7),r9
        add     r9,r0
        mov.l   r0,@(A_RS+8,r6)
5:
        mov.l   @(K_NPTS,r14),r5
        POINT   A_E0
        add     #-3,r5
        cmp/pz  r5
        bt      1f
        bra     .Lfend
        nop
1:
        tst     r5,r5
        bt      .Lfpen
.Lfmid:
        POINT   d
        dt      r5
        bf      .Lfmid
.Lfpen:
        POINT   A_E1
.Lfend:
        mov.l   @(K_ROWS,r14),r0
        tst     r0,r0
        bt      .Lflast
        POINT   row                     ! and the next row's first divide
        mov.l   @(K_ROWS,r14),r0
        add     #-1,r0
        mov.l   r0,@(K_ROWS,r14)
        mov.l   @(K_RI,r14),r0
        add     #1,r0
        bra     .Lfrow
        mov.l   r0,@(K_RI,r14)
.Lflast:
        POINT   last
        ldc.l   @r15+,gbr
        lds.l   @r15+,macl
        lds.l   @r15+,mach
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8
