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
! plane and the camera; a node's faces tested a byte (8 faces) at a time. A
! leaf's box isn't tried: its faces come by its nodes, and the box guarded
! only what's in it, which culls itself (and a model poking into the view
! from a leaf whose box is out of it was lost). r8-r12 are the walk's
! scratch, saved once by the wrapper, not in every node; the box test is
! inline in the node (no call).

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

! one of the view's side planes against the box at r1 (maxs 6 bytes on): r7 at its record;
! the box wholly outside it: to \out
.macro  PLANE bit, out
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
        bra     \out                    ! (too far for BF)
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

_walk_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        sts.l   pr,@-r15
        bsr     .Lwalk
        nop
        lds.l   @r15+,pr
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        mov.l   @r15+,r8
        rts
        nop

.Lwalk:
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
        ! its box against the planes still in the mask (those it's wholly inside come off it),
        ! here rather than called: a call's PR and return a node
        mov     r4,r1
        add     #10,r1                  ! its box
        mov     r6,r7
        add     #W_FR,r7
        PLANE   1, .Lret
        PLANE   2, .Lret
        PLANE   4, .Lret
        PLANE   8, .Lret
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
        bsr     .Lwalk
        mov     r7,r4
        mov.l   @r15+,r5
        mov.l   @r15+,r3
        mov.l   @r15+,r4
        lds.l   @r15+,pr
4:
        ! its faces: those the cluster can see, on the camera's side. A byte (8 faces) at a
        ! time: vis & (side ? back : ~back), the first byte cut below the first face and the
        ! last above the last, then each bit left listed (most bytes have none)
        mov.w   @(8,r4),r0
        extu.w  r0,r2                   ! how many
        tst     r2,r2
        bt      6f
        mov.w   @(6,r4),r0
        extu.w  r0,r1                   ! the first
        mov.l   r4,@-r15
        mov.l   r5,@-r15
        add     r1,r2                   ! one past the last
        mov     r1,r0
        and     #7,r0
        mov     r0,r7
        mova    .Llom,r0
        mov.b   @(r0,r7),r10
        extu.b  r10,r10                 ! the first byte's cut: the faces before the first off
        mov     r1,r0
        shlr2   r0
        shlr    r0
        mov     r0,r1                   ! the byte
        mov.l   @(W_FVIS,r6),r4
        mov.l   @(W_FBACK,r6),r5
5:      mov     r1,r0
        mov.b   @(r0,r5),r11            ! on their planes' backs
        mov.b   @(r0,r4),r0             ! those the cluster can see
        tst     r3,r3
        bf      51f
        not     r11,r11                 ! the camera in front: the fronts
51:     and     r11,r0
        and     r10,r0
        extu.b  r0,r11                  ! the byte's candidates
        mov     #-1,r10                 ! (the next byte isn't cut below)
        mov     r1,r12
        shll2   r12
        shll    r12                     ! the byte's first face
        mov     r2,r0
        sub     r12,r0                  ! faces to the end from it
        mov     #8,r7
        cmp/ge  r7,r0
        bt      52f
        mov     r0,r7
        mova    .Lhim,r0
        mov.b   @(r0,r7),r0
        and     r0,r11                  ! the last byte: the faces past the last off
52:     tst     r11,r11
        bt      55f                     ! none (left) in this byte
        shlr    r11
        bf      54f                     ! not this one
        mov.l   @(W_NLIST,r6),r7        ! listed
        mov.l   @r7,r0
        mov.w   .Lmaxvis,r8
        cmp/hs  r8,r0
        bt      54f
        mov.l   @(W_LMODEL,r6),r8
        mov     #0,r9
        mov.b   r9,@(r0,r8)             ! (the world)
        mov.l   @(W_LIST,r6),r8
        mov     r0,r9
        add     r9,r9
        add     r9,r8
        mov.w   r12,@r8
        add     #1,r0
        mov.l   r0,@r7
        mov.l   @(W_PUB,r6),r7
        mov.l   r0,@r7                  ! for the slave, drawing already
54:     bra     52b
        add     #1,r12
55:     add     #1,r1                   ! the next byte, if its first face is before the end
        mov     r1,r0
        shll2   r0
        shll    r0
        cmp/hs  r2,r0
        bf      5b
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
        bra     .Lwalk
        mov     r7,r4
.Lret:
        rts
        nop

.Lleaf:
        ! the leaf: anything in it (entities, sprites, brush models)? Then the C: extra(leaf,
        ! mask). (Its box isn't tried: see above)
        not     r4,r0
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

        .align  2
.Lmaxvis:
        .short  2048                    ! MAX_VIS
        .short  0
.Llom:  .byte   0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE0, 0xC0, 0x80     ! a byte's faces from the nth on
.Lhim:  .byte   0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F     ! its first n
