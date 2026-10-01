; SCU DSP: the walls' dynamic lights, 1 of 3 (src/render.c dsp_walls): which
; faces this frame's lights may light.
;
; The models' program (xformm.dsp) loads this one over itself when it's done;
; it starts at 255 (a NOP), then 0. The host gives it a list (the whole faces
; the CPUs drew last frame: their indices, two to a word; if the first is the
; second half of its word, W_HALF is 1 and that word's in I_W already) and
; this frame's lights, 10 words each
; (16.16, the world), then 8 units, 5, 6:
;
;   x y z, radius, x y z + reach, reach - x y z     (reach: radius + 2 units)
;
; A light may light a face near its plane, on its side (face_dlights' test:
; -8 < its distance from the plane < its radius) and near the face: in the
; box of its grid's corners, stretched by its reach each way (face_dlights'
; box, near enough). The faces some light may are listed from the host's
; W_ACC, after a word for how many:
;
;   its record's address (>> 2), the lights that may (a bit each)
;
; while there's room (MAXF: walls1.dsp sees to the room for their blocks and
; lit lights); then walls1.dsp is loaded (none listed: the models' program back,
; which stops at its 255).
;
; Data RAM: RAM0 the record (F), its plane (PL), its numbers; the host's from
; 40 (W_). RAM1 the grid's across, then the box's low corner (LO). RAM2 the
; grid's down, then the box's high corner (HI); the face's side (SGN). RAM3
; the lights (LT), the constants after them; scratch.

F       = 0                             ; the record's first 6: origin x y z, axes|plane,
F_W3    = 3                             ; flags nu nv eu0, eu1 ev0 ev1 lodhi
F_W4    = 4
PL      = 6                             ; its plane: n x y z, dist
I_FA    = 10                            ; the entry out: the record's address,
I_MASK  = 11                            ; the lights that may light it
I_FL    = 13                            ; (out of the record's bytes, in this order)
I_NU    = 14
I_NV    = 15
I_MP    = 19                            ; the lights near its plane
I_BIT   = 20                            ; this light's bit,
I_LB    = 21                            ; its place in RAM3
I_W     = 25                            ; the list's word: two faces' indices
W_NJ    = 40                            ; (faces listed: walls1.dsp's)
W_NL1   = 46                            ; lights - 1
W_AX    = 47                            ; the cart's axes >> 2
W_PLN   = 54                            ; the cart's planes >> 2
W_PW1   = 55                            ; walls1.dsp >> 2
W_P0    = 57                            ; the models' program >> 2
W_L0    = 58                            ; the list's next word >> 2,
W_N0    = 59                            ; its entries left,
W_FB    = 60                            ; the cart's faces >> 2
W_HALF  = 61                            ; 1: the next entry's the second half of the word in I_W
W_ACC   = 62                            ; the list out >> 2
W_LT0   = 63                            ; this program's lights >> 2
LO      = 0                             ; RAM1
HI      = 0                             ; RAM2
SGN     = 8                             ; 1, or -1 if the face is on its plane's back
LT      = 0                             ; RAM3
C8      = 30                            ; 8 units (16.16), 5, 6
C5      = 31
C6      = 32
SCR     = 40
MAXF    = 60                            ; (the list's room: walls1.dsp sees to the rest)

; this light's bit and place: the next light's
.macro nextlt
        mov I_BIT,ct0
        mov m0,a
        sl  mov all,mc0
        mov m0,a  mov 10,pl
        btm
        add  mov all,mc0
.endm

walls0: mov W_LT0,ct0
        mov m0,ra0
        mov LT,ct3
        dma d0,mc3,33                   ; the lights (3 at most), the constants
w0:     jmp t0,w0
        nop
        mov W_NJ,ct0
        mov 0,mc0                       ; nothing listed yet
        mov W_ACC,ct0
        mov m0,a  mov 1,pl
        add  mov all,wa0                ; (after the word for how many)

list:   mov W_N0,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0                ; an entry fewer
        jmp s,done                      ; (none left)
        mov W_HALF,ct0
        mov m0,a
        xor  mov all,mc0                ; (the other half next)
        jmp z,half2                     ; (the second half's turn)
        mov I_W,ct0
        mov W_L0,ct0                    ; a new word: its first half
        mov m0,ra0
        mov m0,a
        add  mov all,mc0                ; (the list's next)
        mov I_W,ct0
        dma d0,mc0,1
w1:     jmp t0,w1
        nop
        mov I_W,ct0
        mov m0,a  mov 0,pl
        jmp rec
        ad2  mov alh,pl
half2:  mov m0,a
        mvi $FFFF,pl
        and  mov all,pl
rec:    clr a                           ; the face's index
        add  mov alu,a
        sl   mov alu,a
        sl   mov alu,a
        sl   mov alu,a  mov W_FB,ct0
        mov m0,p  mov I_FA,ct0
        add  mov all,mc0                ; its record's address (8 words each)
        mov I_FA,ct0
        mov m0,ra0
        mov F,ct0
        dma d0,mc0,6                    ; its record's first 6
w2:     jmp t0,w2
        nop
        ; flags nu nv out of their bytes (8 bits round at a time)
        mvi 255,pl
        mov F_W4,ct0
        mov m0,a  mov I_FL,ct0
        rl8  mov alu,a
        and  mov all,mc0
        rl8  mov alu,a
        and  mov all,mc0
        rl8  mov alu,a
        and  mov all,mc0
        ; its plane (5 words an entry), and its side of it
        mov C5,ct3
        mov m3,y
        mvi $FFFF,pl
        mov F_W3,ct0
        mov m0,a
        and  mov all,rx
        mov W_PLN,ct0
        mov mul,p  mov m0,a
        add  mov all,ra0
        mov PL,ct0
        dma d0,mc0,4
w3:     jmp t0,w3
        nop
        mov I_FL,ct0
        mov m0,a  mov 64,pl
        and  mov SGN,ct2
        mvi 1,mc2,z
        mov SGN,ct2
        mvi -1,mc2,nz

        ; the lights near its plane: -8 < d < radius, d its distance from it,
        ; its side of it
        mov I_MP,ct0
        mov 0,mc0
        mov 1,mc0
        mov 0,mc0
        mov W_NL1,ct0
        mov m0,lop
        mov pt,top
pt:     mov I_LB,ct0
        mov m0,ct3
        mov PL,ct0
        mov mc0,x  mov mc3,y  mov SGN,ct2
        mov mul,p  clr a  mov mc0,x  mov mc3,y
        ad2  mov alu,a  mov mul,p  mov mc0,x  mov mc3,y
        ad2  mov alu,a  mov mul,p
        ad2  mov alh,pl                 ; n . light
        mov mc0,a
        sub  mov all,rx  mov m2,y
        mov mul,p  mov mc3,a            ; -d ; radius
        add                             ; radius - d
        jmp zs,pno
        mov C8,ct3
        mov m3,a
        sub                             ; 8 + d
        jmp zs,pno
        nop
        mov I_MP,ct0
        mov m0,a  mov I_BIT,ct0
        mov m0,p  mov I_MP,ct0
        or  mov all,mc0
pno:    nextlt
        mov I_MP,ct0
        mov m0,a  mov 0,pl
        or
        jmp z,list                      ; none: the next face
        nop

        ; its axes (6 words an entry): du into RAM1, dv into RAM2
        mov C6,ct3
        mov m3,y
        mov F_W3,ct0
        mov m0,a  mov 0,pl
        ad2  mov alh,rx
        mov W_AX,ct0
        mov mul,p  mov m0,a
        add  mov all,ra0
        mov LO,ct1
        dma d0,mc1,3
w4:     jmp t0,w4
        nop
        mov HI,ct2
        dma d0,mc2,3
w5:     jmp t0,w5
        nop
        ; the grid across and down (at most): du nu, dv nv
        mov I_NU,ct0
        mov m0,y
        mov LO,ct1
        mov 2,lop
        mov pa,top
pa:     mov m1,x
        mov mul,p  clr a
        btm
        ad2  mov all,mc1
        mov I_NV,ct0
        mov m0,y
        mov HI,ct2
        mov 2,lop
        mov pb,top
pb:     mov m2,x
        mov mul,p  clr a
        btm
        ad2  mov all,mc2
        ; the box of its corners, each way: lo = o + across + down - M,
        ; hi = o + M, M their positive parts' sum
        mov F,ct0
        mov LO,ct1
        mov HI,ct2
        mov SCR,ct3
        mov 2,lop
        mov bx,top
bx:     mov m1,a  mov 0,pl
        add
        mvi 1,rx,ns
        mvi 0,rx,s
        mov m1,y
        mov mul,p  clr a
        ad2  mov all,mc3
        mov m2,a  mov 0,pl
        add
        mvi 1,rx,ns
        mvi 0,rx,s
        mov m2,y
        mov mul,p  mov SCR,ct3
        mov m3,a
        add  mov all,mc3                ; M
        mov m0,a  mov m1,p
        add  mov alu,a  mov m2,p
        add  mov alu,a  mov SCR,ct3
        mov m3,p
        sub  mov all,mc1                ; lo
        mov mc0,a
        btm
        add  mov all,mc2                ; hi

        ; of the lights near its plane, those whose reach its box is in
        mov I_MASK,ct0
        mov 0,mc0
        mov I_BIT,ct0
        mov 1,mc0
        mov 4,mc0
        mov W_NL1,ct0
        mov m0,lop
        mov bt,top
bt:     mov I_MP,ct0
        mov m0,a  mov I_BIT,ct0
        mov m0,p  mov I_LB,ct0
        and
        jmp z,bno
        mov m0,ct3
        mov LO,ct1
        mov HI,ct2
        mov mc3,a  mov mc1,p
        sub  mov mc3,a  mov mc1,p       ; x + reach - lo x
        jmp s,bno
        sub  mov mc3,a  mov mc1,p       ; y
        jmp s,bno
        sub  mov mc3,a  mov mc2,p       ; z
        jmp s,bno
        add  mov mc3,a  mov mc2,p       ; reach - x + hi x
        jmp s,bno
        add  mov mc3,a  mov mc2,p       ; y
        jmp s,bno
        add                             ; z
        jmp s,bno
        nop
        mov I_MASK,ct0
        mov m0,a  mov I_BIT,ct0
        mov m0,p  mov I_MASK,ct0
        or  mov all,mc0
bno:    nextlt
        mov I_MASK,ct0
        mov m0,a  mov 0,pl
        or
        jmp z,list                      ; none
        nop

        ; room for it?
        mov W_NJ,ct0
        mov m0,a  mov MAXF,pl
        sub
        jmp ns,done
        nop
        mov 1,pl
        add  mov all,mc0                ; a face more
        mov I_FA,ct0
        dma mc0,d0,2                    ; its entry
w6:     jmp t0,w6
        nop
        jmp list
        nop

done:   mov W_ACC,ct0
        mov m0,wa0
        mov W_NJ,ct0
        dma mc0,d0,1                    ; how many, before them
w7:     jmp t0,w7
        nop
        mov W_NJ,ct0
        mov m0,a  mov 0,pl
        or  mov W_PW1,ct0
        jmp nz,go
        nop
        mov W_P0,ct0                    ; none: the models' program back (it stops at its 255)
go:     mov m0,ra0
        jmp load
        nop

        .org 253
load:   dma d0,prg,256
        mvi 0,pc
        nop
