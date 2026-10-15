! A dynamic light's weight on a model's 162 normals, in SH-2 assembly (src/model.c
! ents_light_dyn's and src/render.c draw_model's inner loop, 80 cycles a normal as the
! compiler had it: OVERNIGHT.md 89):
!
!   void mlight_asm(u16 *gt, const s16 *nrm, const s16 *k)
!
! For each normal n (2.14, x y z): dot = n . k[0..2] (the direction to the light in the
! model's space, 2.14); w = 5734 + (dot > 0 ? (dot * 10650) >> 14 : 0) (0.35 + 0.65 dot,
! 2.14); each colour of gt[n] (5 bits, packed r | g << 5 | b << 10 | 0x8000) gains
! (k[3 + c] * w) >> 18 and is clamped to 31, where k[3..5] are the light's colours (5
! bits) times its strength at the model (0..65536), >> 12: (c * f * w) >> 30 as the C's
! (c * ((f * w) >> 14)) >> 16, rounding a unit apart at most. MAC.W does the dot, MULS.W
! the rest (all the operands fit 16 bits), a table the clamp.

        .text
        .align  2
        .global _mlight_asm

_mlight_asm:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15
        mov     r6,r8                   ! k: the direction (3 words), then the colours
        mov.w   .L10650,r9
        mov.w   .L5734,r10
        mov.w   @(6,r8),r0              ! the colours' strengths (displaced loads land in r0 only)
        mov     r0,r11
        mov.w   @(8,r8),r0
        mov     r0,r12
        mov.w   @(10,r8),r0
        mov     r0,r13
        mov.l   .Ltab,r14               ! min(a, 31) for a = 0..63, as u16
        mov     #81,r7                  ! 162 normals (an immediate is 8 bits signed)
        add     r7,r7
.Lloop:
        mov     r8,r6
        clrmac
        mac.w   @r5+,@r6+
        mac.w   @r5+,@r6+
        mac.w   @r5+,@r6+
        sts     macl,r0                 ! the dot, 4.28 (unit vectors: under 2^28)
        shll2   r0
        swap.w  r0,r0
        exts.w  r0,r0                   ! >> 14: 2.14
        mov     r10,r1                  ! w = 5734...
        cmp/pl  r0
        bf      .Lback
        muls.w  r0,r9                   ! ...+ (dot * 10650) >> 14
        sts     macl,r0
        shll2   r0
        swap.w  r0,r0
        exts.w  r0,r0
        add     r0,r1
.Lback:
        mov.w   @r4,r2                  ! g0: the colours so far
        ! red: (g0 & 31) + (kr * w) >> 18, clamped
        muls.w  r11,r1
        mov     r2,r0
        and     #31,r0
        sts     macl,r6
        shlr16  r6
        shlr2   r6
        add     r6,r0
        add     r0,r0
        mov.w   @(r0,r14),r3
        ! green: ((g0 >> 5) & 31) + (kg * w) >> 18, clamped, << 5
        muls.w  r12,r1
        mov     r2,r0
        shlr2   r0
        shlr2   r0
        shlr    r0
        and     #31,r0
        sts     macl,r6
        shlr16  r6
        shlr2   r6
        add     r6,r0
        add     r0,r0
        mov.w   @(r0,r14),r6
        shll2   r6
        shll2   r6
        shll    r6
        or      r6,r3
        ! blue: ((g0 >> 10) & 31) + (kb * w) >> 18, clamped, << 10, and the 0x8000
        muls.w  r13,r1
        mov     r2,r0
        shlr8   r0
        shlr2   r0
        and     #31,r0
        sts     macl,r6
        shlr16  r6
        shlr2   r6
        add     r6,r0
        add     r0,r0
        mov.w   @(r0,r14),r6
        shll8   r6
        shll2   r6
        or      r6,r3
        mov.w   .L8000,r0
        or      r0,r3
        mov.w   r3,@r4
        dt      r7
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
.L10650: .word  10650
.L5734: .word   5734
.L8000: .word   0x8000
        .align  2
.Ltab:  .long   _mlight_clamp

! (low work RAM: high is full; 128 bytes, cached once read)
        .section .lwrodata
        .align  2
        .global _mlight_clamp
_mlight_clamp:
        .word   0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        .word   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
        .word   31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31
        .word   31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31
