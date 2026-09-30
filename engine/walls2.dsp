; SCU DSP: the walls' dynamic lights, 2 of 2 (src/render.c dsp_walls): each
; face's lights with the dynamic lights added, from walls1.dsp's blocks.
;
; walls1.dsp loads this one over itself; it starts at 255 (a NOP), then 0;
; at the end it loads the models' program back the same way (which stops at
; its 255), so the CPUs never do.
; For each block: the face's lights (the cart's copy, a word holds two), each
; grid point's position (origin + b_j dvt + a_i dut), and at it each light's
; share added, as dl_face does:
;
;   d = floor(point - light) (whole units, each way), d2 = d.d
;   in reach (d2 < r2): f = ((r2 - d2) inv) >> 8, and (c f) >> 16 added to
;   each of r g b; then each clamped at 31, packed as a Gouraud colour
;
; The lit lights out one face after another from the host's W_OUT, two to a
; word (an odd face's last word has one). The host's lights, 8 words each (at
; most 3): -x -y -z (16.16), r2, -(inv << 8), r g b.
;
; Data RAM: RAM0 the block's first 14 (H), a point's position and sums (PS),
; the rest's; the host's from 40 (W_). RAM1 the lights (LT), the row's start
; (R), the b_j (BL), scratch (S1). RAM2 the face's lights as read, then the
; a_i dut; the origin (O2); constants (C2); scratch (SC); at the end the lit
; lights, two to a word. RAM3 the face's lights, one a point.

H       = 0                             ; nu nv ow k0 rw raw aw bw, origin (3), dvt (3)
PS      = 14                            ; x y z (16.16), r g b sums
I_BP    = 20                            ; the block's address
I_BJ    = 21                            ; this row's b_j's place
I_J     = 22                            ; rows after this one
I_AI    = 23                            ; this point's a_i dut's place
I_PL    = 24                            ; points after this one
W_NJ2   = 53                            ; (the host's) faces
W_BLK   = 42                            ; the blocks >> 2
W_OUT   = 43                            ; the lit lights >> 2
W_LT    = 44                            ; the lights >> 2
W_LTW   = 45                            ; their words
W_NL1   = 46                            ; lights - 1
W_C7FFF = 56                            ; 0x7FFF
W_P0    = 57                            ; the models' program (xformm.dsp) >> 2
LT      = 0                             ; RAM1
R       = 24
BL      = 28
S1      = 60
O2      = 36                            ; RAM2
C2      = 48                            ; 31 0x3E0 0x7C00 2048 64 32 1024 0x8000 65536
SC      = 60

walls2: mov W_LT,ct0
        mov mc0,ra0
        mov LT,ct1
        dma d0,mc1,m0                   ; the lights (CT0 on to their words)
w0:     jmp t0,w0
        nop
        mov C2,ct2
        mvi 31,mc2
        mvi $3E0,mc2
        mvi $7C00,mc2
        mvi 2048,mc2
        mvi 64,mc2
        mvi 32,mc2
        mvi 1024,mc2
        mvi $8000,mc2
        mvi 65536,mc2
        mov W_BLK,ct0
        mov m0,a  mov 0,pl
        mov I_BP,ct0
        add  mov all,mc0                ; (the first block)
        mov W_OUT,ct0
        mov m0,wa0                      ; (the lit lights, one face after another)

job:    mov I_BP,ct0
        mov m0,ra0
        mov H,ct0
        dma d0,mc0,14                   ; the block's first 14
w1:     jmp t0,w1
        nop
        ; the face's lights: rw words, then a point a word into RAM3 (the low 15 bits)
        mov H+5,ct0
        mov m0,ra0
        mov 0,ct2
        mov H+4,ct0
        dma d0,mc2,m0
w2:     jmp t0,w2
        nop
        mov H+4,ct0
        mov m0,a  mov 1,pl
        sub  mov all,lop
        mov W_C7FFF,ct0
        mov m0,p
        mov 0,ct2
        mov lpw,top
        mov mc2,a  mov 0,ct3
lpw:    rl8  mov alu,a
        rl8  mov alu,a
        and  mov all,mc3
        rl8  mov alu,a
        rl8  mov alu,a
        btm
        and  mov all,mc3  mov mc2,a
        ; the offsets: a_i dut into RAM2, b_j into RAM1
        mov I_BP,ct0
        mov m0,a  mov 14,pl
        add  mov all,ra0
        mov 0,ct2
        mov H+6,ct0
        dma d0,mc2,m0
w3:     jmp t0,w3
        nop
        mov BL,ct1
        mov H+7,ct0
        dma d0,mc1,m0
w4:     jmp t0,w4
        nop
        ; the next block: 14 + aw + bw on
        mov I_BP,ct0
        mov m0,a  mov 14,pl
        add  mov alu,a  mov H+6,ct0
        mov mc0,p
        add  mov alu,a
        mov m0,p  mov I_BP,ct0
        add  mov all,mc0
        ; the origin into RAM2 (the rows' starts come from it and dvt)
        mov H+8,ct0
        mov O2,ct2
        mov mc0,mc2
        mov mc0,mc2
        mov mc0,mc2

        mov H+1,ct0
        mov m0,a  mov 0,pl
        mov I_J,ct0
        add  mov all,mc0                ; rows after the first: nv
        mov I_BJ,ct0
        mov BL,mc0
        mov H+3,ct0
        mov m0,ct3                      ; the first point's light in RAM3

row:    ; its start: origin + b_j dvt (RAM1 R)
        mov I_BJ,ct0
        mov m0,ct1
        mov mc1,x
        mov m0,a  mov 1,pl
        add  mov all,mc0                ; (the next row's b_j)
        mov H+11,ct0
        mov O2,ct2
        mov R,ct1
        mov mc0,y
        mov mul,p  mov mc2,a
        ad2  mov all,mc1  mov mc0,y
        mov mul,p  mov mc2,a
        ad2  mov all,mc1  mov mc0,y
        mov mul,p  mov mc2,a
        ad2  mov all,mc1
        mov I_AI,ct0
        mov 0,mc0
        mov H,ct0
        mov m0,a  mov 0,pl
        mov I_PL,ct0
        add  mov all,mc0                ; points after the first: nu

pt:     ; the point: the row's start + a_i dut (PS)
        mov I_AI,ct0
        mov m0,ct2
        mov R,ct1
        mov PS,ct0
        mov mc1,a  mov mc2,p
        ad2  mov all,mc0  mov mc1,a  mov mc2,p
        ad2  mov all,mc0  mov mc1,a  mov mc2,p
        ad2  mov all,mc0
        mov I_AI,ct0
        mov m0,a  mov 3,pl
        add  mov all,mc0
        ; its sums: its light's r g b (5 bits each)
        mov PS+3,ct0
        mov C2,ct2
        mov S1,ct1
        mov m3,a  mov mc2,p
        and  mov all,mc0  mov mc2,p     ; r
        and  mov all,rx  mov mc2,p
        and  mov all,mc1  mov mc2,y
        mov mul,p  clr a  mov mc2,y  mov S1,ct1
        ad2  mov alh,mc0  mov m1,x      ; g
        mov mul,p
        ad2  mov alh,mc0                ; b

        ; each light's share (RAM1: -x -y -z r2 -inv<<8 r g b)
        mov W_NL1,ct0
        mov m0,lop
        mov LT,ct1
        mov lgt,top
lgt:    mov PS,ct0
        mov mc0,a  mov mc1,p  mov SC,ct2
        ad2  mov alh,mc2  mov mc0,a  mov mc1,p   ; d x (whole units)
        ad2  mov alh,mc2  mov mc0,a  mov mc1,p   ; d y
        ad2  mov alh,mc2  clr a                  ; d z
        mov SC,ct2
        mov m2,y
        mov mc2,rx
        mov mul,p  mov m2,y
        ad2  mov alu,a  mov mc2,rx
        mov mul,p  mov m2,y
        ad2  mov alu,a  mov mc2,rx
        mov mul,p
        ad2  mov alu,a  mov mc1,p                ; d2 ; r2
        sub  mov all,rx  mov mc1,y               ; d2 - r2 (in reach: < 0) ; -inv << 8
        jmp s,in
        clr a
        mov 0,rx                                 ; (out of reach: nothing added)
in:     mov mul,p  mov PS+3,ct0
        ad2  mov alh,rx  mov mc1,y               ; f ; r
        mov mul,p  clr a  mov mc1,y
        ad2  mov alh,pl  mov m0,a
        add  mov all,mc0  mov mul,p  clr a  mov mc1,y
        ad2  mov alh,pl  mov m0,a
        add  mov all,mc0  mov mul,p  clr a
        ad2  mov alh,pl  mov m0,a
        btm
        add  mov all,mc0

        ; each sum clamped at 31, packed as a Gouraud colour, over the point's light
        mvi 32,pl
        mov PS+3,ct0
        mov m0,a
        sub
        mvi 31,mc0,ns
        mov PS+4,ct0
        mov m0,a
        sub
        mvi 31,mc0,ns
        mov PS+5,ct0
        mov m0,a
        sub
        mvi 31,mc0,ns
        mov PS+3,ct0
        mov C2+5,ct2
        mov mc0,a
        mov mc0,x  mov mc2,y
        mov mul,p  mov mc0,x  mov mc2,y
        ad2  mov alu,a  mov mul,p
        ad2  mov alu,a  mov mc2,p
        ad2  mov all,mc3

        mov I_PL,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0
        jmp ns,pt
        nop
        mov I_J,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0
        jmp ns,row
        nop

        ; out, two to a word
        mov H+3,ct0
        mov m0,ct3
        mov C2+8,ct2
        mov m2,y
        mov 0,ct2
        mov H+2,ct0
        mov m0,a  mov 1,pl
        sub  mov all,lop
        mov lpp,top
lpp:    mov mc3,x
        mov mul,p  mov mc3,a
        btm
        ad2  mov all,mc2
        mov 0,ct2
        mov H+2,ct0
        dma mc2,d0,m0
w5:     jmp t0,w5
        nop

        mov W_NJ2,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0                ; a face fewer
        jmp nz,job
        nop
        mov W_P0,ct0
        mov m0,ra0
        jmp load                        ; all lit: the models' program back (it stops at its 255)
        nop

        .org 253
load:   dma d0,prg,256
        mvi 0,pc
        nop
