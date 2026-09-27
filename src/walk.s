! The BSP walk in SH-2 assembly (src/render.c's walk, which it replaces):
! front to back from a node, the faces facing the camera that its cluster can
! see, listed for the drawing.
!
!   void walk_asm(int n, int mask, const walk_ctx *w)
!
! n: a node (>= 0) or leaf (~n) already known to be in the PVS. mask: the
! view's side planes its box is across (1 2 4 8: those it's wholly inside
! needn't be tried on anything in it). Leaves holding anything (entities,
! sprites, brush models) go to the C, w->extra(leaf, mask).
!
! Quicker than the compiler's: the child's PVS test before the call rather
! than after it; only what's live across the near child's walk saved (node,
! side, mask and PR), the far child a jump; the box tests unrolled with the
! mask bits as immediates; the plane's side with MAC.L straight from the
! plane and the camera. Only r0-r7 (and r8 r9 in a face loop, saved) are used.

        .text
        .align  2
        .global _walk_asm

W_NODES  = 0
W_PLANES = 4
W_LEAFS  = 8
W_NVIS   = 12                           ! node_vis (u16s)
W_LVIS   = 16                           ! leaf_vis
W_FRAME  = 20
W_FVIS   = 24                           ! the faces visible from the cluster (bits)
W_FBACK  = 28                           ! on their plane's back (bits)
W_CAM    = 32                           ! x y z
W_BITM   = 44                           ! 1 2 4 ... 128
W_LIST   = 48                           ! vis_faces (u16s)
W_LMODEL = 52                           ! vis_model (u8s)
W_NLIST  = 56                           ! &nvis
W_PUB    = 60                           ! &published (uncached)
W_FR     = 64                           ! 4 planes, 24 bytes: the far corner's offsets (x y z), the near's, 2 spare, n x y z, d
W_LEXTRA = 160                          ! leaf_ent leaf_spr leaf_model, the C for a leaf with any, the list's size

! is the node or leaf in \reg in the PVS? T. Uses r0 r1
.macro  INPVS reg
        mov     \reg,r0
        cmp/pz  r0
        bt      81f
        not     r0,r0                   ! a leaf: ~n
        add     r0,r0
        mov.l   @(W_LVIS,r6),r1
        bra     82f
        mov.w   @(r0,r1),r0
81:     add     r0,r0
        mov.l   @(W_NVIS,r6),r1
        mov.w   @(r0,r1),r0
82:     extu.w  r0,r0
        mov.l   @(W_FRAME,r6),r1
        cmp/eq  r1,r0
.endm

! one of the view's side planes against the box at r1 (maxs 6 bytes on): r7 at its record
.macro  PLANE bit
        mov     r5,r0
        tst     #\bit,r0
        bt      89f                     ! not across it
        ! the corner furthest along the plane's normal: outside, and so's all of the box
        mov.b   @(0,r7),r0
        mov.w   @(r0,r1),r0
        mov.l   @(8,r7),r2
        mul.l   r0,r2
        mov.b   @(1,r7),r0
        sts     macl,r3
        mov.w   @(r0,r1),r0
        mov.l   @(12,r7),r2
        mul.l   r0,r2
        mov.b   @(2,r7),r0
        sts     macl,r2
        add     r2,r3
        mov.w   @(r0,r1),r0
        mov.l   @(16,r7),r2
        mul.l   r0,r2
        mov.l   @(20,r7),r0
        sts     macl,r2
        add     r2,r3
        cmp/ge  r0,r3
        bt      88f
        bra     .Lout                   ! (too far for BF)
        nop
88:
        ! the nearest: inside, and so's all of it (the plane comes off the mask)
        mov.b   @(3,r7),r0
        mov.w   @(r0,r1),r0
        mov.l   @(8,r7),r2
        mul.l   r0,r2
        mov.b   @(4,r7),r0
        sts     macl,r3
        mov.w   @(r0,r1),r0
        mov.l   @(12,r7),r2
        mul.l   r0,r2
        mov.b   @(5,r7),r0
        sts     macl,r2
        add     r2,r3
        mov.w   @(r0,r1),r0
        mov.l   @(16,r7),r2
        mul.l   r0,r2
        mov.l   @(20,r7),r0
        sts     macl,r2
        add     r2,r3
        cmp/ge  r0,r3
        bf      89f
        mov     r5,r0
        and     #(15 - \bit),r0
        mov     r0,r5
89:     add     #24,r7
.endm

! is the box at r1 outside one of the planes in r5? T. The planes it's wholly
! inside come off r5. Uses r0 r2 r3 r7 and MACL
cull:
        mov     r6,r7
        add     #W_FR,r7
        PLANE   1
        PLANE   2
        PLANE   4
        PLANE   8
        rts
        clrt
.Lout:
        rts
        sett

_walk_asm:
        cmp/pz  r4
        bt      0f
        bra     .Lleaf
        nop
0:
        ! the node: nodes + n * 24
        mov     r4,r1
        shll2   r1
        shll    r1
        mov     r1,r0
        add     r1,r1
        add     r0,r1
        mov.l   @(W_NODES,r6),r4
        add     r1,r4
        tst     r5,r5
        bt      1f
        mov     r4,r1
        add     #10,r1                  ! its box
        sts.l   pr,@-r15
        bsr     cull
        nop
        lds.l   @r15+,pr
        bf      1f
        rts                             ! out of the view
        nop
1:
        ! which side of its plane the camera's on
        mov.w   @r4,r0
        extu.w  r0,r0
        mov     r0,r1
        shll2   r1
        mov     r1,r2
        shll2   r2
        add     r2,r1                   ! the plane * 20
        mov.l   @(W_PLANES,r6),r2
        add     r1,r2
        mov.w   @(16,r2),r0             ! type << 8 | signbits
        shlr8   r0
        mov     #3,r1
        cmp/hs  r1,r0
        bt      2f
        shll2   r0                      ! on an axis: the camera's coordinate less dist
        add     #W_CAM,r0
        mov.l   @(r0,r6),r1
        mov.l   @(12,r2),r3
        bra     3f
        sub     r3,r1
2:      clrmac                          ! n . the camera (16.16 by 16.16: the middle 32 of the 64), less dist
        mov     r6,r3
        add     #W_CAM,r3
        mac.l   @r2+,@r3+
        mac.l   @r2+,@r3+
        mac.l   @r2+,@r3+
        sts     mach,r1
        sts     macl,r3
        xtrct   r1,r3
        mov.l   @r2,r1
        sub     r1,r3
        mov     r3,r1
3:      shll    r1
        movt    r3                      ! the side: behind the plane
        ! the near child
        mov     r3,r0
        add     r0,r0
        add     #2,r0
        mov.w   @(r0,r4),r7
        INPVS   r7
        bf      4f
        sts.l   pr,@-r15
        mov.l   r4,@-r15
        mov.l   r3,@-r15
        mov.l   r5,@-r15
        bsr     _walk_asm
        mov     r7,r4
        mov.l   @r15+,r5
        mov.l   @r15+,r3
        mov.l   @r15+,r4
        lds.l   @r15+,pr
4:
        ! its faces: those the cluster can see, on the camera's side
        mov.w   @(8,r4),r0
        extu.w  r0,r2                   ! how many
        tst     r2,r2
        bt      6f
        mov.w   @(6,r4),r0
        extu.w  r0,r1                   ! the first
        mov.l   r4,@-r15
        mov.l   r5,@-r15
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   @(W_FVIS,r6),r4
        mov.l   @(W_FBACK,r6),r5
5:      mov     r1,r0
        and     #7,r0
        mov.l   @(W_BITM,r6),r7
        mov.b   @(r0,r7),r7
        extu.b  r7,r7                   ! its bit
        mov     r1,r0
        shlr2   r0
        shlr    r0                      ! its byte
        mov.b   @(r0,r5),r8             ! on its plane's back?
        mov.b   @(r0,r4),r0             ! can the cluster see it?
        tst     r7,r0
        bt      7f
        and     r7,r8
        tst     r8,r8
        movt    r8                      ! on the front
        cmp/eq  r3,r8                   ! facing away (front and the camera behind, or back and in front)
        bt      7f
        mov.l   @(W_NLIST,r6),r7        ! listed
        mov.l   @r7,r0
        mov.w   .Lmaxvis,r8
        cmp/hs  r8,r0
        bt      7f
        mov.l   @(W_LMODEL,r6),r8
        mov     #0,r9
        mov.b   r9,@(r0,r8)             ! (the world)
        mov.l   @(W_LIST,r6),r8
        mov     r0,r9
        add     r9,r9
        add     r9,r8
        mov.w   r1,@r8
        add     #1,r0
        mov.l   r0,@r7
        mov.l   @(W_PUB,r6),r7
        mov.l   r0,@r7                  ! for the slave, drawing already
7:      add     #1,r1
        dt      r2
        bf      5b
        mov.l   @r15+,r9
        mov.l   @r15+,r8
        mov.l   @r15+,r5
        mov.l   @r15+,r4
6:
        ! the far child: in this frame (a jump)
        mov     r3,r0
        xor     #1,r0
        add     r0,r0
        add     #2,r0
        mov.w   @(r0,r4),r7
        INPVS   r7
        bf      .Lret
        bra     _walk_asm
        mov     r7,r4
.Lret:
        rts
        nop

.Lleaf:
        ! the leaf: leafs + l * 28
        not     r4,r0
        mov     r0,r1
        shll2   r1
        mov     r1,r2
        add     r1,r1
        add     r1,r2
        add     r1,r1
        add     r2,r1
        mov.l   @(W_LEAFS,r6),r2
        add     r2,r1
        add     #4,r1                   ! its box
        tst     r5,r5
        bt      1f
        sts.l   pr,@-r15
        mov.l   r0,@-r15
        bsr     cull
        nop
        mov.l   @r15+,r0
        lds.l   @r15+,pr
        bt      .Lret
1:      ! anything in it? (entities, sprites, brush models: the C)
        mov     r0,r4                   ! (the leaf, for the C)
        add     r0,r0
        mov     r6,r7
        add     #100,r7
        add     #(W_LEXTRA - 100),r7
        mov.l   @r7,r1
        mov.w   @(r0,r1),r2
        mov.l   @(4,r7),r1
        mov.w   @(r0,r1),r3
        and     r3,r2
        mov.l   @(8,r7),r1
        mov.w   @(r0,r1),r3
        and     r3,r2
        cmp/pz  r2                      ! any list's head >= 0: something
        bf      .Lret
        mov.l   @(12,r7),r1
        sts.l   pr,@-r15                ! extra(leaf, mask), in C: which can use r0-r7,
        mov.l   r6,@-r15                ! and the walk above needs r6 (the context) after
        jsr     @r1
        nop
        mov.l   @r15+,r6
        lds.l   @r15+,pr
        rts
        nop

        .align  1
.Lmaxvis:
        .short  2048                    ! MAX_VIS
