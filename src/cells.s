! A row of a face's cells, in SH-2 assembly (src/render.c's draw_face calls
! it for each row): each cell's VDP1 command and Gouraud table.
!
!   void cells_asm(cell_args *a)
!
! a->rows rows of a->n cells (a whole face's grid, one row after another, or
! a row at a time for a big face's).
!
! It does the cells that are most of them: a whole tile or a crop that's
! exactly its grid cell (q_cell_fast: everything the command needs is in
! the record), all in front of the near plane, its texture in VRAM; and
! small crops inside their grid cell (.Lcrop: their corners interpolated
! on screen, as cell_corners does). It skips the empty ones and those
! outside the view (all four corners outside one plane). The rest (near
! the camera, big crops, a texture to load) it lists for the C, by number
! in the face (from a->cell0), in a->def.
!
! The command's LINK: pushed (the slave's, drawn last-first), each points
! at the one before; appended (the master's), the one before is pointed at
! this. a->prev is that one before: its LINK value, or its address.
!
! Registers: r4 r5 the grid's top and bottom rows (gv, 8 bytes), r6 the
! cell, r7 the top row's lights, r8 the cells left, r9 where the command
! goes and r10 its LINK, r11 the one before, r12 where the Gouraud table
! goes and r13 its GRDA, r14 the args; r0-r3 as they come.

        .text
        .align  2
        .global _cells_asm

C_TEXSLOT = 0                           ! this CPU's texture -> slot (u16s)
C_SFRAME  = 4                           ! the slots' last frame used (u16s)
C_FRAME   = 8
C_SB8     = 12                          ! a slot's bytes / 8
C_BASE8   = 16                          ! the slots' VRAM / 8
C_LUT8    = 20                          ! the colour tables' VRAM / 8
C_PMOD    = 24                          ! PMOD << 16
C_CTRL    = 28                          ! CTRL << 16
C_FIFO    = 32                          ! appended?
C_STRIDE2 = 36                          ! bytes from a light to the one below
C_FAST    = 40                          ! CELL_FULL | CELL_EXACT
C_NEAR    = 44                          ! (OC_NEAR | OC_FAR) << 24
C_N       = 48                          ! cells in the row
C_DEFP    = 52                          ! where the next deferred cell's number goes (u16)
C_ROWS    = 56                          ! rows left
C_ROW     = 60                          ! top bot cell light cmd link prev gst grda cell0
C_CROP    = 100                         ! (then, from r14 + 100:) cl0 cr1 ct0 cb1 rows0 N rcp slot_lut slot_w
INTERP_PX = 64                          ! a crop smaller than this on the screen: interpolated

G_XY    = 0                             ! (gv)
G_OC    = 4                             ! the outcode: the top byte of the word here
G_SIZE  = 8

.Lcrop_t:
        bra     .Lcrop                  ! (from the loop: .Lcrop is out of a short branch's reach)
        nop

_cells_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        sts.l   macl,@-r15
        mov     r4,r14
        mov     r14,r1
        add     #C_ROW,r1
        mov.l   @r1,r4
        mov.l   @(4,r1),r5
        mov.l   @(8,r1),r6
        mov.l   @(12,r1),r7
        mov.l   @(16,r1),r9
        mov.l   @(20,r1),r10
        mov.l   @(24,r1),r11
        mov.l   @(28,r1),r12
        mov.l   @(32,r1),r13
        mov.l   @(C_N,r14),r8
.Lcell:
        mov.w   @r6,r0                  ! tex
        cmp/eq  #-1,r0
        bt      .Lnext                  ! empty
        mov     r0,r3
        ! outside one plane at all four corners: nothing of it's seen
        mov.l   @(G_OC,r4),r0
        mov.l   @(G_OC+G_SIZE,r4),r1
        and     r1,r0
        mov.l   @(G_OC+G_SIZE,r5),r1
        and     r1,r0
        mov.l   @(G_OC,r5),r1
        and     r1,r0
        shlr16  r0
        shlr8   r0
        and     #31,r0                  ! (not OC_FAR: far isn't outside)
        tst     r0,r0
        bf      .Lnext
        ! behind the near plane, or far off the screen, at any corner: the C clips it or
        ! splits it
        mov.l   @(G_OC,r4),r0
        mov.l   @(G_OC+G_SIZE,r4),r1
        or      r1,r0
        mov.l   @(G_OC+G_SIZE,r5),r1
        or      r1,r0
        mov.l   @(G_OC,r5),r1
        or      r1,r0
        mov.l   @(C_NEAR,r14),r1
        tst     r1,r0
        bf      .Ldefer
        ! a whole tile or an exact crop (the others: the C)
        mov.l   @(C_FAST,r14),r1
        tst     r1,r3
        bt      .Lcrop_t                ! a crop inside its grid cell: small ones in .Lcrop
        ! its texture's slot (none: the C loads it)
        mov.w   .Lmask,r0
        and     r3,r0
        add     r0,r0
        mov.l   @(C_TEXSLOT,r14),r1
        mov.w   @(r0,r1),r0
        cmp/eq  #-1,r0
        bt      .Ldefer
        mov     r0,r2                   ! the slot
        add     r0,r0
        mov.l   @(C_SFRAME,r14),r1
        mov.l   @(C_FRAME,r14),r3
        mov.w   r3,@(r0,r1)             ! used this frame
        ! SRCA: the slot's, and where the first row drawn starts; SIZE: width / 8, rows
        mov.l   @(C_SB8,r14),r1
        mulu.w  r2,r1
        mov.l   @(4,r6),r1              ! ty0 off8 wsz th
        mov.l   @(C_BASE8,r14),r3
        mov     r1,r0
        shlr16  r0
        extu.b  r0,r0
        add     r0,r3
        sts     macl,r0
        add     r0,r3
        shll16  r3
        extu.w  r1,r1
        or      r1,r3
        mov.l   r3,@(8,r9)
        ! PMOD, COLR (the colour table: 32 bytes, so its number x 4 in 8s); transposed?
        mov.w   @(2,r6),r0
        shll16  r0
        shll    r0                      ! T: TEX_TRANSPOSED
        movt    r2
        shlr16  r0
        add     r0,r0
        mov.l   @(C_LUT8,r14),r1
        add     r1,r0
        mov.l   @(C_PMOD,r14),r1
        or      r1,r0
        mov.l   r0,@(4,r9)
        ! the corners: A top[i], C bot[i+1], and B D top[i+1] bot[i] (the other way round
        ! if the texture's rows run down the cell)
        mov.l   @(G_XY,r4),r0
        mov.l   r0,@(12,r9)
        mov.l   @(G_XY+G_SIZE,r5),r0
        mov.l   r0,@(20,r9)
        mov.l   @(G_XY+G_SIZE,r4),r0
        mov.l   @(G_XY,r5),r1
        tst     r2,r2
        bf      1f
        mov.l   r0,@(16,r9)
        bra     2f
        mov.l   r1,@(24,r9)
1:      mov.l   r1,@(16,r9)
        mov.l   r0,@(24,r9)
2:
.Lgouraud:
        ! the Gouraud table: the corners' lights in the same order
        mov.w   @r7,r1                  ! light[i]
        mov.w   @(2,r7),r0              ! light[i + 1]
        shll16  r1
        extu.w  r0,r0
        or      r0,r1                   ! A B
        mov.l   @(C_STRIDE2,r14),r0
        mov.w   @(r0,r7),r3             ! D: the row below's light[i]
        add     #2,r0
        mov.w   @(r0,r7),r0             ! C: its light[i + 1]
        shll16  r0
        extu.w  r3,r3
        or      r3,r0                   ! C D
        tst     r2,r2
        bt      3f
        mov     r1,r3                   ! transposed: A D, C B (swap the low halves)
        xor     r0,r3
        extu.w  r3,r3
        xor     r3,r1
        xor     r3,r0
3:      mov.l   r1,@r12
        mov.l   r0,@(4,r12)
        add     #8,r12
        mov     r13,r0
        shll16  r0
        mov.l   r0,@(28,r9)             ! GRDA
        add     #1,r13
        ! CTRL and LINK
        mov.l   @(C_CTRL,r14),r0
        mov.l   @(C_FIFO,r14),r1
        tst     r1,r1
        bf      4f
        or      r11,r0                  ! pushed: to the one before
        mov.l   r0,@r9
        bra     5f
        mov     r10,r11
4:      mov.l   r0,@r9                  ! appended: the one before to this
        tst     r11,r11
        bt      6f
        mov     r10,r0
        mov.w   r0,@(2,r11)
6:      mov     r9,r11
5:      add     #32,r9
        add     #4,r10
.Lnext:
        add     #G_SIZE,r4
        add     #G_SIZE,r5
        add     #8,r6
        dt      r8
        bf/s    .Lcell
        add     #2,r7
        ! the row's done: the next (past the row's last point and light)
        mov.l   @(C_ROWS,r14),r0
        add     #-1,r0
        tst     r0,r0
        bt/s    .Ldone
        mov.l   r0,@(C_ROWS,r14)
        add     #G_SIZE,r4
        add     #G_SIZE,r5
        add     #2,r7
        bra     .Lcell
        mov.l   @(C_N,r14),r8
.Ldone:
        ! out: where the next command and table go
        mov     r14,r1
        add     #C_ROW,r1
        mov.l   r9,@(16,r1)
        mov.l   r10,@(20,r1)
        mov.l   r11,@(24,r1)
        mov.l   r12,@(28,r1)
        mov.l   r13,@(32,r1)
        lds.l   @r15+,macl
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

.Ldefer:
        ! for the C: its number in the face
        mov     r14,r1
        add     #C_ROW,r1
        mov.l   @(36,r1),r0             ! cell0
        mov     r6,r1
        sub     r0,r1
        shlr2   r1
        shlr    r1
        mov.l   @(C_DEFP,r14),r0
        mov.w   r1,@r0
        add     #2,r0
        bra     .Lnext
        mov.l   r0,@(C_DEFP,r14)

        .align  1
.Lmask: .short  0x3FFF

! one corner of a crop: fa (u) and fb (v) across the grid cell A B C D
! (r4 r5 r6 r7, packed x << 16 | y), out to dst. Uses r0-r3
.macro  CROPCORNER fa, fb, dst
        ! along the top and bottom edges to the crop's column, then down it to its row
        swap.w  r4,r0
        exts.w  r0,r0                   ! xA
        swap.w  r5,r1
        exts.w  r1,r1
        sub     r0,r1                   ! xB - xA
        mul.l   \fa,r1
        sts     macl,r1
        shlr16  r1
        exts.w  r1,r1
        add     r0,r1                   ! tx
        swap.w  r7,r0
        exts.w  r0,r0                   ! xD
        swap.w  r6,r2
        exts.w  r2,r2
        sub     r0,r2                   ! xC - xD
        mul.l   \fa,r2
        sts     macl,r2
        shlr16  r2
        exts.w  r2,r2
        add     r0,r2                   ! bx
        sub     r1,r2
        mul.l   \fb,r2
        sts     macl,r2
        shlr16  r2
        exts.w  r2,r2
        add     r1,r2                   ! x
        shll16  r2
        exts.w  r4,r0                   ! yA
        exts.w  r5,r1
        sub     r0,r1
        mul.l   \fa,r1
        sts     macl,r1
        shlr16  r1
        exts.w  r1,r1
        add     r0,r1                   ! ty
        exts.w  r7,r0                   ! yD
        exts.w  r6,r3
        sub     r0,r3
        mul.l   \fa,r3
        sts     macl,r3
        shlr16  r3
        exts.w  r3,r3
        add     r0,r3                   ! by
        sub     r1,r3
        mul.l   \fb,r3
        sts     macl,r3
        shlr16  r3
        exts.w  r3,r3
        add     r1,r3                   ! y
        extu.w  r3,r3
        or      r3,r2
        mov.l   r2,\dst
.endm

! |a - b| of the halves of two packed points (x: hi, y: lo) under INTERP_PX, or the C. Uses r0-r2
.macro  SMALL pa, pb
        mov.l   \pa,r0
        mov.l   \pb,r1
        swap.w  r0,r2
        exts.w  r2,r2
        swap.w  r1,r8
        exts.w  r8,r8
        sub     r8,r2
        cmp/pz  r2
        bt      51f
        neg     r2,r2
51:     mov     #INTERP_PX,r8
        cmp/ge  r8,r2
        bt      .Lcrop_c
        exts.w  r0,r2
        exts.w  r1,r1
        sub     r1,r2
        cmp/pz  r2
        bt      52f
        neg     r2,r2
52:     cmp/ge  r8,r2
        bt      .Lcrop_c
.endm

.Lcrop_c:
        bra     .Ldefer                 ! (not here: the C)
        mov.l   @r15+,r8
.Lcrop:
        ! A crop inside its grid cell (not the grid cell exactly: cell_corners'
        ! small case, the same sums): all in front of the near plane (the
        ! test above), small on the screen, its texture in VRAM; else the C
        mov.l   r8,@-r15                ! (r8 the cells left in the row: used as a scratch here)
        SMALL   "@(G_XY,r4)", "@(G_XY+G_SIZE,r5)"       ! A to C
        SMALL   "@(G_XY+G_SIZE,r4)", "@(G_XY,r5)"       ! B to D
        mov     r3,r0                   ! (a crop: no CELL_FULL, CELL_EXACT bits to mask off)
        add     r0,r0
        mov.l   @(C_TEXSLOT,r14),r1
        mov.w   @(r0,r1),r0
        cmp/eq  #-1,r0
        bt      .Lcrop_c                ! not in VRAM: the C loads it
        extu.w  r0,r3                   ! the slot
        mov.l   r13,@-r15
        mov.l   r12,@-r15
        mov.l   r11,@-r15
        mov.l   r10,@-r15
        mov.l   r7,@-r15
        mov.l   r6,@-r15
        mov.l   r5,@-r15
        mov.l   r4,@-r15                ! @(0,r15) r4 .. @(28,r15) r13, @(32,r15) r8
        ! used this frame; SRCA (the slot and the rows it starts down it), SIZE, PMOD and COLR
        mov     r3,r0
        add     r0,r0
        mov.l   @(C_SFRAME,r14),r1
        mov.l   @(C_FRAME,r14),r2
        mov.w   r2,@(r0,r1)
        mov     r14,r10
        add     #C_CROP,r10             ! the crop's fields
        mov.l   @(32,r10),r1            ! slot_w
        mov     r3,r0
        mov.b   @(r0,r1),r4
        extu.b  r4,r4                   ! tw
        mov.l   @(28,r10),r1            ! slot_lut
        add     r0,r0
        mov.w   @(r0,r1),r5
        extu.w  r5,r5                   ! tl
        mov.l   @(C_SB8,r14),r1
        mulu.w  r3,r1
        mov.l   @(C_BASE8,r14),r0
        sts     macl,r1
        add     r0,r1
        shll2   r1
        add     r1,r1                   ! the slot's VRAM (bytes)
        mov.b   @(2,r6),r0              ! ty0
        extu.b  r0,r0
        mov     r4,r2
        shlr    r2                      ! tw / 2: bytes a row
        mulu.w  r0,r2
        sts     macl,r0
        add     r0,r1
        shlr2   r1
        shlr    r1
        shll16  r1                      ! SRCA
        mov     r4,r0
        shlr2   r0
        shlr    r0
        shll8   r0                      ! (tw / 8) << 8
        mov     r0,r2
        mov.b   @(3,r6),r0
        extu.b  r0,r0                   ! th
        or      r2,r0
        or      r0,r1
        mov.l   r1,@(8,r9)
        mov     r5,r0                   ! the colour table (its number x 4 in 8s); the texture's
        shll16  r0                      ! rows running down the cell?
        shll    r0                      ! T: TEX_TRANSPOSED
        movt    r13                     ! (kept till the corners are out)
        shlr16  r0
        add     r0,r0
        mov.l   @(C_LUT8,r14),r1
        add     r1,r0
        mov.l   @(C_PMOD,r14),r1
        or      r1,r0
        mov.l   r0,@(4,r9)
        ! its grid cell's texels: the face's edge columns and rows are narrower
        mov.l   @(20+12,r15),r0         ! the cells left in the row (r8, pushed first)
        mov.l   @(C_N,r14),r1
        cmp/eq  r1,r0
        movt    r2
        neg     r2,r2
        mov.l   @(0,r10),r3
        and     r2,r3                   ! cl: cl0 in the first column, else 0
        mov     #1,r1
        cmp/eq  r1,r0
        bt/s    1f
        mov.l   @(4,r10),r4             ! cr: cr1 in the last column
        mov.l   @(20,r10),r4            ! else N
1:      mov.l   @(C_ROWS,r14),r0
        mov.l   @(16,r10),r1
        cmp/eq  r1,r0
        movt    r2
        neg     r2,r2
        mov.l   @(8,r10),r5
        and     r2,r5                   ! ct: ct0 in the first row
        mov     #1,r1
        cmp/eq  r1,r0
        bt/s    2f
        mov.l   @(12,r10),r7            ! cb: cb1 in the last row
        mov.l   @(20,r10),r7            ! else N
2:      ! the crop's fractions across it, 16.16 (a reciprocal, not a divide); its own edges exactly 1
        mov.l   @(24,r10),r8            ! rcp
        mov     r4,r0
        sub     r3,r0                   ! w = cr - cl
        shll2   r0
        mov.l   @(r0,r8),r1             ! rcp[w]
        mov.b   @(4,r6),r0
        extu.b  r0,r0                   ! u0
        sub     r3,r0
        mul.l   r1,r0
        sts     macl,r10                ! fa0
        mov.b   @(5,r6),r0
        extu.b  r0,r0                   ! u1
        cmp/eq  r4,r0
        bf/s    3f
        sub     r3,r0
        mov     #1,r11
        bra     4f
        shll16  r11                     ! 65536
3:      mul.l   r1,r0
        sts     macl,r11                ! fa1
4:      mov     r7,r0
        sub     r5,r0                   ! h = cb - ct
        shll2   r0
        mov.l   @(r0,r8),r1             ! rcp[h]
        mov.b   @(6,r6),r0
        extu.b  r0,r0                   ! v0
        sub     r5,r0
        mul.l   r1,r0
        sts     macl,r12                ! fb0
        mov.b   @(7,r6),r0
        extu.b  r0,r0                   ! v1
        cmp/eq  r7,r0
        bf/s    5f
        sub     r5,r0
        mov     #1,r2
        bra     6f
        shll16  r2
5:      mul.l   r1,r0
        sts     macl,r2                 ! fb1
6:      mov     r2,r8                   ! fb1 (the reciprocals are done with)
        ! the grid cell's corners: A B top[i] top[i+1], C D bot[i+1] bot[i]
        mov.l   @(0,r15),r0             ! top
        mov.l   @(4,r15),r1             ! bot
        mov.l   @(G_XY,r0),r4
        mov.l   @(G_XY+G_SIZE,r0),r5
        mov.l   @(G_XY+G_SIZE,r1),r6
        mov.l   @(G_XY,r1),r7
        CROPCORNER r10, r12, "@(12,r9)" ! (fa0, fb0): A
        CROPCORNER r11, r8, "@(20,r9)"  ! (fa1, fb1): C
        CROPCORNER r11, r12, "@-r15"    ! (fa1, fb0): B, and
        CROPCORNER r10, r8, "@-r15"     ! (fa0, fb1): D aside
        ! B and D: the other way round if the texture's rows run down the cell
        mov.l   @r15+,r1                ! D
        mov.l   @r15+,r0                ! B
        tst     r13,r13
        bt      7f
        mov     r0,r2
        mov     r1,r0
        mov     r2,r1
7:      mov.l   r0,@(16,r9)
        mov.l   r1,@(24,r9)
        mov     r13,r2                  ! (the Gouraud table's order, as the fast path's)
        mov.l   @r15+,r4
        mov.l   @r15+,r5
        mov.l   @r15+,r6
        mov.l   @r15+,r7
        mov.l   @r15+,r10
        mov.l   @r15+,r11
        mov.l   @r15+,r12
        mov.l   @r15+,r13
        mov.l   @r15+,r8
        bra     .Lgouraud
        nop
