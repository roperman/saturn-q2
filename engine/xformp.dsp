; SCU DSP batch transform of packed vertices, a matrix per block of 16
;
;   out = M_b * v + t_b     block b, 16 vertices each
;
; As xformb.dsp, but the vertices are MD2's: one word each, bytes x y z n
; (high to low), unpacked here (rotate left 8, and with 255) so the models'
; frames don't need expanding. The host writes a header per block:
;
;   t0 m00 m01 m02  t1 m10 m11 m12  t2 m20 m21 m22  vaddr     (13 words)
;
; vaddr (address >> 2) points at the block's 16 packed vertices (they can
; be on the cart: the DSP's DMA reaches the A-bus). Results go out
; sequentially: per block 16 x', 16 y', 16 z'. int * 16.16 lands in the
; low 32 bits (ALL) with no shifting, so with M already scaled by the
; frame's scale, the results are 16.16 view space.
;
; Data RAM:
;   RAM0[0..12]  the current header
;   RAM0[16]     stream address >> 2
;   RAM0[17]     output address >> 2
;   RAM0[18]     block count (>= 1), counted down
;   RAM0[19]     where the next header is
;   RAM3         the block's packed vertices, RAM1 unpacked (x y z), RAM2 results
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
        mov m0,ra0                      ; the packed vertices
        mov 0,ct3
        dma d0,mc3,#BLOCK
w_in:   jmp t0,w_in
        mov 0,ct3                       ; (delay slot)

        ; unpack: RAM3 word -> RAM1 x, y, z. The ALU's result only exists in
        ; the instruction that makes it (the manual's "ad2 mov alu,a"), so each
        ; shift or mask and the move that takes it share an instruction; the
        ; ALU works from A as it was, so the last one also loads the next word
        mov 0,ct1
        mvi 255,pl                      ; the mask, in P
        mvi BLOCK-1,lop
        mov u_loop,top
        mov mc3,a    mov 0,ct2          ; A = x y z n; the rows write RAM2 from 0
u_loop: rl8  mov alu,a                  ; A = y z n x
        and  mov all,mc1                ; x out
        rl8  mov alu,a                  ; A = z n x y
        and  mov all,mc1                ; y out
        rl8  mov alu,a                  ; A = n x y z
        btm
        and  mov mc3,a  mov all,mc1     ; z out, the next word in (the delay slot: every pass)

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
