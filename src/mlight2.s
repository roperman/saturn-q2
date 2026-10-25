! A dynamic light's weight on a model's 162 normals, the second way (OPT=-DMLIGHT_V2; src/mlight.s
! is the first): the three colours added and clamped together rather than one at a time
! (OVERNIGHT.md 106).
!
!   void mlight2_asm(u16 *gt, const s16 *nrm, const s16 *k, const u32 *tab)
!
! k is the direction to the light in the model's space (2.14, 3 words). The dot product of each
! normal with it (MAC.W, 4.28) is quantised to 32 steps of a 32nd, and tab[step] holds what that
! step adds to the colours, worked out once a light (src/model.c mlight): its low word red and
! blue in a 5-5-5 word's places (bits 0-4 and 10-14, each at most 31), its high word green in
! its (bits 5-9). The colours so far, gt[n] (r | g << 5 | b << 10 | 0x8000), are split the same
! way: y = red and blue (the five zero bits between them and bit 15 take the carries), z = green
! (bits 10 up take its). One add each, then a lane that went over 31 (its carry bit set: 0x20,
! 0x8000 or 0x400) is set to 31: m = t - (t >> 5) is 0x1F in that lane's place, (y | m) - t
! fills it and clears the carry. Nothing to clamp (the common case) skips it. 35 instructions a
! normal against the first way's 64, the same colours a unit apart at most (the step's rounding).

        .text
        .align  2
        .global _mlight2_asm

_mlight2_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        mov     r6,r8                   ! k: the direction (3 words)
        mov.w   .L7C1F,r9               ! red and blue's places
        mov.w   .L03E0,r10              ! green's
        mov.w   .L8020,r12              ! red's and blue's carry bits
        mov.w   .L0400,r13              ! green's
        mov.w   .L8000,r14
        mov     #81,r11                 ! 162 normals (an immediate is 8 bits signed)
        add     r11,r11
.Lloop:
        mov     r8,r6
        clrmac
        mac.w   @r5+,@r6+
        mac.w   @r5+,@r6+
        mac.w   @r5+,@r6+
        mov.w   @r4,r2                  ! the colours so far (while the MAC finishes)
        sts     macl,r0                 ! the dot, 4.28 (unit vectors: under 2^28)
        cmp/pz  r0
        bt      .Lpos
        mov     #0,r0                   ! facing away: the first step (the ambient share)
.Lpos:
        shlr16  r0                      ! >> 23: the step, 0..31
        shlr2   r0
        shlr2   r0
        shlr2   r0
        shlr    r0
        shll2   r0
        mov.l   @(r0,r7),r1             ! what the step adds: red and blue (low), green (high)
        mov     r2,r3
        and     r9,r3                   ! y: red and blue so far
        and     r10,r2                  ! z: green so far
        extu.w  r1,r0
        add     r0,r3                   ! y += its adds
        shlr16  r1
        add     r1,r2                   ! z += its
        mov     r3,r0
        and     r12,r0                  ! t: the carries out of red and blue
        mov     r2,r1
        and     r13,r1                  ! and out of green
        or      r1,r0
        tst     r0,r0
        bt      .Ldone                  ! (none over 31: the common case)
        mov     r3,r0
        and     r12,r0                  ! y's t
        mov     r0,r1
        shlr2   r1
        shlr2   r1
        shlr    r1                      ! t >> 5
        mov     r0,r6
        sub     r1,r6                   ! m = t - (t >> 5): 0x1F in the lane's place
        or      r6,r3
        sub     r0,r3                   ! (y | m) - t: the lane 31, its carry gone
        mov     r2,r0
        and     r13,r0                  ! z's t
        mov     r0,r1
        shlr2   r1
        shlr2   r1
        shlr    r1
        mov     r0,r6
        sub     r1,r6
        or      r6,r2
        sub     r0,r2
.Ldone:
        or      r2,r3
        or      r14,r3                  ! the 0x8000
        mov.w   r3,@r4
        dt      r11
        bf/s    .Lloop
        add     #2,r4
        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        rts
        mov.l   @r15+,r8

        .align  2
.L7C1F: .word   0x7C1F
.L03E0: .word   0x03E0
.L8020: .word   0x8020
.L0400: .word   0x0400
.L8000: .word   0x8000
