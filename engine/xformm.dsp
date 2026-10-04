; SCU DSP: models' vertices into view space, both frames at once
;
;   out = C + A0 * v0 + A1 * v1        per vertex, 16 at a time
;
; v0, v1: the vertex in the two frames being blended (MD2's packed bytes x y z
; n, as xformp.dsp unpacks them); A0, A1: each frame's matrix, already scaled
; by its share of the blend, so the blend's done here too. For every model
; the host writes a header of 25 words:
;
;   t0 a00 a01 a02 b00 b01 b02   t1 a10 ...   t2 a20 ...   (3 rows of 7)
;   vaddr0  vaddr1  blocks  out                             (addresses >> 2)
;
; and the results go out block by block from out (work RAM-H, or the cart
; when that's full): 16 x', 16 y', 16 z' (16.16). After
; each model it adds one to a count in work RAM, so a CPU needing a model
; only waits for that one. int * 16.16 lands in the low 32 bits (ALL).
;
; Data RAM:
;   RAM0[0..24]   the model's header (vaddr0, vaddr1 and blocks counted on)
;   RAM0[32]      the next header's address >> 2
;   RAM0[33]      where the next block goes >> 2
;   RAM0[34]      models left (>= 1)
;   RAM0[35]      the count's address >> 2
;   RAM0[36]      the count
;   RAM3          the packed vertices (0..15 frame 0, 16..31 frame 1), then the results
;   RAM1, RAM2    frame 0's and frame 1's, unpacked (x y z a vertex)
;   RAM0[40..63]  the walls' job's (engine/walls1.dsp), which this program
;                 starts when it's done, if it has faces (RAM0[40])
;
; JMP and BTM have a delay slot. An ALU result exists only in the instruction
; that makes it (the manual's AD2 MOV ALU,A), so each op and its move share one.

BLOCK   = 16
HEAD    = 25
H_OUT   = 24
P_NEXT  = 32
P_OUT   = 33
P_LEFT  = 34
P_CADDR = 35
P_COUNT = 36
W_NJ    = 40                            ; (the walls' job: faces, and its first program >> 2)
W_P1    = 51
W_MKP   = 56                            ; the texture maker's block >> 2 (engine/make.dsp; 0: none)

; a row of the results: t + a . v0 + b . v1, six products a vertex
.macro rowpass base
        mov 0,ct1
        mov 0,ct2
        mov \base,ct0
        mov mc1,x    mov mc0,a          mov BLOCK-1,lop
        mov r\@,top
r\@:    mov mc0,y
        mov mul,p    mov mc1,x    mov mc0,y
        ad2 mov mul,p   mov mc1,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov mc2,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov mc2,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov mc2,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov alu,a    mov \base,ct0
        btm
        ad2 mov mc1,x   mov mc0,a    mov all,mc3
.endm

model:  mov P_NEXT,ct0
        mov m0,ra0
        mov 0,ct0
        dma d0,mc0,#HEAD                ; the header
w_head: jmp t0,w_head
        nop
        mov P_NEXT,ct0
        mov m0,a
        mov HEAD,pl
        add  mov all,mc0                ; the next header's 25 words on
        mov H_OUT,ct0
        mov m0,a  mov 0,pl
        mov P_OUT,ct0
        or  mov all,mc0                 ; where its blocks go

nextb:  mov 21,ct0
        mov m0,ra0
        mov 0,ct3
        dma d0,mc3,#BLOCK               ; frame 0's 16 vertices
w_in0:  jmp t0,w_in0
        nop
        mov 22,ct0
        mov m0,ra0
        dma d0,mc3,#BLOCK               ; frame 1's, after them
w_in1:  jmp t0,w_in1
        nop
        mov 21,ct0
        mov m0,a
        mov BLOCK,pl
        add  mov all,mc0                ; the next block's: 16 words on
        mov m0,a
        add  mov all,mc0

        ; unpack: RAM3 0..15 -> RAM1, 16..31 -> RAM2 (x y z each)
        mov 0,ct3
        mov 0,ct1
        mvi 255,pl                      ; the mask
        mvi BLOCK-1,lop
        mov u0,top
        mov mc3,a    mov 0,ct2
u0:     rl8  mov alu,a                  ; A = y z n x
        and  mov all,mc1                ; x
        rl8  mov alu,a
        and  mov all,mc1                ; y
        rl8  mov alu,a
        btm
        and  mov mc3,a  mov all,mc1     ; z, and the next word (frame 1's first, at the end)
        mvi BLOCK-1,lop
        mov u1,top
u1:     rl8  mov alu,a
        and  mov all,mc2
        rl8  mov alu,a
        and  mov all,mc2
        rl8  mov alu,a
        btm
        and  mov mc3,a  mov all,mc2

        mov 0,ct3
        rowpass 0
        rowpass 7
        rowpass 14

        mov P_OUT,ct0
        mov m0,wa0
        mov 0,ct3
        dma mc3,d0,#48                  ; out (in one burst: three of 16 measured slower on a Saturn,
w_out:  jmp t0,w_out                    ; OVERNIGHT.md 69)
        nop
        mov P_OUT,ct0
        mov m0,a
        mov 48,pl
        add  mov all,mc0
        mov 23,ct0
        mov m0,a
        mov 1,pl
        sub  mov all,mc0                ; a block fewer
        jmp nz,nextb
        nop

        ; the model's done: count it, out where the CPUs can see
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
        sub  mov all,mc0                ; a model fewer
        jmp nz,model
        nop

        ; the walls' job next, if the host gave it faces: its first program
        ; (engine/walls1.dsp) over this one, by the DSP's own DMA into program
        ; RAM; which goes on at 255 (a NOP), then 0
        mov W_NJ,ct0
        mov m0,a  mov 0,pl
        or
        jmp z,mdone
walls:  mov W_P1,ct0                    ; (the host starts here for the walls alone)
        mov m0,ra0
        jmp load
        nop
mdone:  mov W_MKP,ct0                   ; no walls: the texture maker, if the host has one
        mov m0,a  mov 0,pl
        or
        jmp z,mend
        mov m0,ra0
        jmp load
        nop
mend:   end

        .org 253
load:   dma d0,prg,256                  ; (the next program: RA0)
        mvi 0,pc                        ; (from 0; and on, when it's in, at 255)
        end                             ; (at 255: walls2.dsp loads this program back when it's done, and stops here)
