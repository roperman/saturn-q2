; SCU DSP: the walls' dynamic lights, 1 of 2 (src/render.c dsp_walls): each
; face's numbers set out for engine/walls2.dsp, which lights it.
;
; The models' program (xformm.dsp) loads this one over itself when it's done,
; by the DSP's own DMA into program RAM; it starts at 255 (a NOP), then 0.
; For each face in the host's list (its record's address on the cart), it
; reads the record and its grid's axes (the cart's copies) and writes a block
; (from the host's W_BLK, one after another):
;
;   nu nv ow k0 rw raw aw bw        points a row - 1, rows - 1, words out; where
;                                   the lights start in their first word (0, 1),
;                                   words in, their first (>> 2); the offsets' words
;   origin (3), dvt (3)             (16.16, the world)
;   a_i dut (aw = 3 (nu + 1))       each column's offset (16.16)
;   b_j (bw = nv + 1)               each row's, in texels
;
; with a_0 = 0, a_i = i N - eu0, a_nu = (nu - 1) N - eu0 + eu1 (the grid's
; outer lines are on the face's edges), dut = du (65536 / N) >> 16 (dl_face's
; steps: grid_step's); b the same. Then it loads walls2.dsp the same way.
;
; Data RAM: RAM0 the record (F), the axes then steps (AX), the block's first
; 8 (H), the rest's; the host's from 40 (W_). RAM1 the a_i (AL), the b_j (BL).
; RAM2 the a_i dut.

F       = 0                             ; the record: origin x y z, axes|plane, flags nu nv eu0,
F_W3    = 3                             ; eu1 ev0 ev1 lodhi, cells, firstlight
F_W4    = 4
F_W5    = 5
F_W7    = 7
AX      = 8                             ; du dv, then dut dvt
E_EU0   = 14                            ; (eu0, then nu nv straight into H)
H       = 15                            ; nu nv ow k0 rw raw aw bw
I_EU1   = 23
I_EV0   = 24
I_EV1   = 25
I_SP    = 26                            ; (the last offset)
I_FA    = 27                            ; (the record's address)
W_NJ    = 40                            ; (the host's) faces
W_JOB   = 41                            ; the list's next >> 2
W_BLK   = 42                            ; the blocks >> 2
W_AX    = 47                            ; the cart's axes >> 2
W_RAW   = 48                            ; the cart's lights >> 2
W_N     = 49                            ; N
W_RCP   = 50                            ; 65536 / N
W_P2    = 52                            ; walls2.dsp >> 2
W_C3    = 54                            ; 3
W_C6    = 55                            ; 6
AL      = 0                             ; RAM1
BL      = 16

; the offsets along a way (n cells, the first starting e0 in, the last ending e1
; in), in texels: 0, i N - e0, ..., the last (n - 1) N - e0 + e1; n + 1 into
; RAM1 at list
.macro offsets n, e0, e1, list
        mov W_N,ct0
        mov m0,y                        ; RY = N
        mov \n,ct0
        mov m0,x                        ; RX = n
        mov \e1,ct0
        mov m0,a
        mov \e0,ct0
        mov m0,p
        sub  mov alu,a  mov W_N,ct0     ; e1 - e0
        mov m0,p
        sub  mov alu,a                  ; - N
        mov mul,p  mov I_SP,ct0
        add  mov all,mc0                ; + n N: the last
        mov \list,ct1
        mov 0,mc1                       ; the first
        mov \n,ct0
        mov m0,a  mov 1,pl
        sub  mov all,lop                ; (n more)
        mov \e0,ct0
        mov m0,p  clr a
        sub  mov alu,a  mov W_N,ct0     ; -e0
        mov m0,p
        lps
        add  mov alu,a  mov all,mc1     ; i N - e0
        mov \n,ct0
        mov m0,a  mov \list,pl
        add  mov all,ct1
        mov I_SP,ct0
        mov m0,mc1                      ; (and the last, over n N - e0)
.endm

walls1: mov W_BLK,ct0
        mov m0,wa0                      ; (the blocks, one after another)

job:    mov W_JOB,ct0
        mov m0,ra0
        mov I_FA,ct0
        dma d0,mc0,1                    ; the face's record's address
w1:     jmp t0,w1
        nop
        mov I_FA,ct0
        mov m0,ra0
        mov F,ct0
        dma d0,mc0,8                    ; its record
w2:     jmp t0,w2
        nop
        mov W_JOB,ct0
        mov m0,a  mov 1,pl
        add  mov all,mc0                ; (the list's next)

        ; eu0 nu nv, eu1 ev0 ev1 out of their bytes (8 bits round at a time)
        mvi 255,pl
        mov F_W4,ct0
        mov m0,a  mov E_EU0,ct0
        and  mov all,mc0
        rl8  mov alu,a
        rl8  mov alu,a
        and  mov all,mc0
        rl8  mov alu,a
        and  mov all,mc0
        mov F_W5,ct0
        mov m0,a  mov I_EU1,ct0
        rl8  mov alu,a
        and  mov all,mc0
        rl8  mov alu,a
        and  mov all,mc0
        rl8  mov alu,a
        and  mov all,mc0

        ; its lights: the first's word, and which half (k0)
        mvi $FFFFFF,pl
        mov F_W7,ct0
        mov m0,a  mov H+3,ct0
        and  mov alu,a
        mov 1,pl
        and  mov all,mc0                ; k0
        sr   mov alu,a  mov W_RAW,ct0
        mov m0,p  mov H+5,ct0
        add  mov all,mc0                ; raw

        ; aw = 3 (nu + 1), bw = nv + 1; words out (np + 1) / 2, in (np + 1 + k0) / 2
        mov W_C3,ct0
        mov m0,y
        mov H,ct0
        mov m0,a  mov 1,pl
        add  mov all,rx                 ; nu + 1
        mov H+1,ct0
        mov m0,a  mov H+7,ct0
        add  mov all,mc0                ; bw
        mov mul,p  clr a  mov H+6,ct0
        ad2  mov all,mc0                ; aw
        mov H+7,ct0
        mov m0,y
        mov mul,p  clr a  mov H+2,ct0
        ad2  mov alu,a                  ; np
        mov 1,pl
        add  mov alu,a
        sr   mov all,mc0                ; ow
        mov mc0,p
        add  mov alu,a
        sr   mov all,mc0                ; rw

        ; its axes (6 words an entry), then dut dvt = (du dv) (65536 / N) >> 16, in place
        mov F_W3,ct0
        mov m0,a  mov 0,pl
        ad2  mov alh,rx
        mov W_C6,ct0
        mov m0,y
        mov W_AX,ct0
        mov mul,p  mov m0,a
        ad2  mov all,ra0
        mov AX,ct0
        dma d0,mc0,6
w3:     jmp t0,w3
        nop
        mov W_RCP,ct0
        mov m0,y
        mov AX,ct0
        clr a  mov 5,lop
        mov lpd,top
lpd:    mov m0,x
        mov mul,p
        btm
        ad2  mov alh,mc0

        ; the offsets: the columns', then those times dut; the rows'
        offsets H, E_EU0, I_EU1, AL
        mov AL,ct1
        mov 0,ct2
        mov H,ct0
        mov m0,lop
        mov lpa,top
        clr a
lpa:    mov AX,ct0
        mov mc1,x  mov mc0,y
        mov mul,p  mov mc0,y
        ad2  mov all,mc2  mov mul,p  mov mc0,y
        ad2  mov all,mc2  mov mul,p
        btm
        ad2  mov all,mc2
        offsets H+1, I_EV0, I_EV1, BL

        ; the block out
        mov H,ct0
        dma mc0,d0,8
w4:     jmp t0,w4
        nop
        mov F,ct0
        dma mc0,d0,3
w5:     jmp t0,w5
        nop
        mov AX+3,ct0
        dma mc0,d0,3
w6:     jmp t0,w6
        nop
        mov 0,ct2
        mov H+6,ct0
        dma mc2,d0,m0
w7:     jmp t0,w7
        nop
        mov BL,ct1
        mov H+7,ct0
        dma mc1,d0,m0
w8:     jmp t0,w8
        nop

        mov W_NJ,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0                ; a face fewer
        jmp nz,job
        nop
        mov W_P2,ct0
        mov m0,ra0
        jmp load                        ; all set out: walls2.dsp, over this one
        nop

        .org 253
load:   dma d0,prg,256
        mvi 0,pc
        nop
