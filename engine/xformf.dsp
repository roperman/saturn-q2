; SCU DSP: faces' grids into view space (a test: the DSP doing draw_face's setup)
;
;   o = R (origin - cam),   du' = R du,   dv' = R dv
;
; R: the camera's rows (right, up, forward), 16.16; the face's origin and its
; grid axes (from the level's axes table), 16.16 world. 16.16 times 16.16 is
; 32.32: the three products summed in the 48-bit accumulator and the top 32
; (ALH) taken, 16.16 again.
;
; A job: 2 words: the face's record (on the cart: the DSP can't reach low
; work RAM) >> 2, where its 9 results go >> 2. After each, one more on a
; count in work RAM.
;
; Data RAM:
;   RAM0[0..8]    R          RAM0[9..11] the camera
;   RAM0[32]      the next job >> 2      RAM0[33] jobs left (>= 1)
;   RAM0[34]      the count's address >> 2   RAM0[35] the axes table >> 2
;   RAM0[36]      the count  RAM0[40..41] the job  RAM0[43] the axes' number  RAM0[44] 6
;   RAM1[0..3]    the face's origin and (axes << 16 | plane), then d; [4..9] du dv
;   RAM2[0..8]    the results

P_JOB   = 32
P_LEFT  = 33
P_CADDR = 34
P_AXES  = 35
P_COUNT = 36
J_FACE  = 40
J_OUT   = 41
A_NUM   = 43
K_SIX   = 44

; one row of R (at \r) times the vector at RAM1[\v], out to RAM2
.macro row r, v
        mov \v,ct1
        mov \r,ct0
        mov mc1,x    mov mc0,y
        mov mul,p    mov mc1,x    mov mc0,y   clr a
        ad2 mov mul,p   mov mc1,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov alu,a
        ad2 mov alh,mc2
.endm

job:    mov P_JOB,ct0
        mov m0,ra0
        mov J_FACE,ct0
        dma d0,mc0,#2                   ; the job
w_job:  jmp t0,w_job
        nop
        mov P_JOB,ct0
        mov m0,a
        mov 2,pl
        add  mov all,mc0

        mov J_FACE,ct0
        mov m0,ra0
        mov 0,ct1
        dma d0,mc1,#4                   ; the face: its origin, axes << 16 | plane
w_face: jmp t0,w_face
        nop

        ; d = origin - the camera
        mov 0,ct1
        mov 9,ct0
        mov m1,a     mov mc0,p
        sub  mov all,mc1
        mov m1,a     mov mc0,p
        sub  mov all,mc1
        mov m1,a     mov mc0,p
        sub  mov all,mc1

        ; the axes: word 3's top half, times 6 words, into the table
        mov m1,a                        ; (ct1 is 3)
        rl8  mov alu,a
        rl8  mov alu,a
        mvi 65535,pl
        mov A_NUM,ct0
        and  mov all,mc0
        mov A_NUM,ct0
        mov mc0,x                       ; the number
        mov K_SIX,ct0
        mov mc0,y
        mov mul,p
        mov P_AXES,ct0
        mov m0,a
        add  mov all,ra0
        mov 4,ct1
        dma d0,mc1,#6                   ; du, dv
w_axes: jmp t0,w_axes
        nop

        mov 0,ct2
        row 0, 0
        row 3, 0
        row 6, 0
        row 0, 4
        row 3, 4
        row 6, 4
        row 0, 7
        row 3, 7
        row 6, 7

        mov J_OUT,ct0
        mov m0,wa0
        mov 0,ct2
        dma mc2,d0,#9
w_out:  jmp t0,w_out
        nop
        mov P_COUNT,ct0
        mov m0,a
        mov 1,pl
        add  mov all,mc0
        mov P_CADDR,ct0
        mov m0,wa0
        mov P_COUNT,ct0
        dma mc0,d0,#1
w_cnt:  jmp t0,w_cnt
        nop
        mov P_LEFT,ct0
        mov m0,a
        mov 1,pl
        sub  mov all,mc0
        jmp nz,job
        nop
        end
