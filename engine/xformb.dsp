; SCU DSP batch transform with a matrix per block of 16 vertices
;
;   out = M_b * v + t_b     block b, 16 vertices each
;
; For skeletal models: every bone's vertices are padded to whole blocks, and
; one job transforms every bone of every model in a stream. The host writes
; a header per block into the stream (HWRAM):
;
;   t0 m00 m01 m02  t1 m10 m11 m12  t2 m20 m21 m22  vaddr     (13 words)
;
; vaddr (address >> 2) points at the block's 16 vertices: integer x y z
; triples, AoS, which stay put (the model's own table). Results go out
; sequentially: per block 16 x', 16 y', 16 z'. As in xform.dsp, int * 16.16
; lands in the low 32 bits (ALL) with no shifting.
;
; Data RAM:
;   RAM0[0..12]  the current header
;   RAM0[16]     stream address >> 2
;   RAM0[17]     output address >> 2
;   RAM0[18]     block count (>= 1), counted down
;   RAM0[19]     where the next header is (the DMA moves RA0 on, and RA0
;                is reloaded for each block's vertices, so we keep our place)
;   RAM1         the block's vertices, RAM2 its results
;
; JMP and BTM have a delay slot: the instruction after them always executes.

BLOCK   = 16
WORDS   = BLOCK*3
HEAD    = 13
PARAMS  = 16
COUNT   = 18
PTR     = 19

.macro rowpass base
        mov 0,ct1
        mov \base,ct0
        mov mc1,x    mov mc0,a          mov BLOCK-1,lop
        mov r\@,top
r\@:    mov mc0,y
        mov mul,p    mov mc1,x    mov mc0,y
        ad2 mov mul,p   mov mc1,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov alu,a    mov \base,ct0
        btm
        ad2 mov mc1,x   mov mc0,a    mov all,mc2
.endm

        mov PARAMS,ct0
        mov mc0,a                       ; A = the stream
        mov mc0,wa0                     ; WA0 = the output
        mov 0,pl
        mov PTR,ct0
        add  mov all,mc0                ; RAM0[19] = the first header

next:   mov PTR,ct0
        mov m0,ra0
        mov 0,ct0
        dma d0,mc0,#HEAD                ; this block's matrix and vertex address
w_head: jmp t0,w_head
        nop
        mov HEAD,pl
        mov PTR,ct0
        mov m0,a
        add  mov all,mc0                ; the next header is 13 words on
        mov 12,ct0
        mov m0,ra0                      ; the vertices
        mov 0,ct1
        dma d0,mc1,#WORDS
w_in:   jmp t0,w_in
        mov 0,ct2                       ; (delay slot)

        rowpass 0
        rowpass 4
        rowpass 8

        mov 0,ct2
        dma mc2,d0,#WORDS               ; 48 results out, WA0 moves on
w_out:  jmp t0,w_out
        mov COUNT,ct0                   ; (delay slot)
        mov 1,pl
        mov m0,a
        sub  mov all,mc0                ; one block fewer; Z when none left
        jmp nz,next
        nop
        end
