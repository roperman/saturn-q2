; SCU DSP: models' vertices into view space, both frames at once, each model's
; lighting first (OPT=-DDSP_LIGHT: xformm.dsp with the lighting; see below)
;
;   out = C + A0 * v0 + A1 * v1        per vertex, 16 at a time
;
; v0, v1: the vertex in the two frames being blended (MD2's packed bytes x y z
; n, as xformp.dsp unpacks them); A0, A1: each frame's matrix, already scaled
; by its share of the blend, so the blend's done here too. For every model
; the host writes a header of 25 words:
;
;   t0 a00 a01 a02 b00 b01 b02   t1 a10 ...   t2 a20 ...   (3 rows of 7)
;   vaddr0  vaddr1  blocks                                  (addresses >> 2)
;   jobs                                                    (its lighting's, below)
;
; and the results go out block by block: 16 x', 16 y', 16 z' (16.16). After
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
;   RAM0[40..63]  the lighting's (below)
;   RAM3          the packed vertices (0..15 frame 0, 16..31 frame 1), then the results
;   RAM1, RAM2    frame 0's and frame 1's, unpacked (x y z a vertex)
;
; JMP and BTM have a delay slot. An ALU result exists only in the instruction
; that makes it (the manual's AD2 MOV ALU,A), so each op and its move share one.

BLOCK   = 16
HEAD    = 25
L_JOBS  = 40
L_NORM  = 41
L_LEFT  = 42
L_CADDR = 43
L_COUNT = 44
L_CHUNK = 45
L_TOP   = 46
J_OUT   = 55
J_M     = 57
J_W0    = 63
P_NEXT  = 32
P_OUT   = 33
P_LEFT  = 34
P_CADDR = 35
P_COUNT = 36

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

        mov L_TOP,ct0
        mvi el,mc0                      ; (the lighting's element loop: past a D1 immediate's reach)
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
        ; its lighting first (what the CPU wants first), then its vertices
        mov 24,ct0
        mov m0,a
        mov 0,pl
        mov L_LEFT,ct0
        add  mov all,mc0                ; its jobs (none: straight on)
        jmp nz,ljob
        nop

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
        dma mc3,d0,#48                  ; out
w_out:  jmp t0,w_out
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
        end

; ---- each model's lighting, before its vertices: for each job (the model and
; a dynamic light near it) the weight each of the 162 normals takes of the
; light (src/render.c draw_model's):
;
;   w = (f (5734 + t)) >> 14,  t = dot > 0 ? (dot 10650) >> 14 : 0,  dot = (n . m) >> 14
;
; the same numbers as (4f*5734 + 4f t) >> 16, t = (dot 42600) >> 16 and dot =
; (n . 4m) >> 16 (a shift by 16 is ALH, the top of a 48-bit sum), and where dot
; isn't positive, w0 = (f 5734) >> 14. A job, 9 words from the host:
;
;   out>>2  -  4mx 4my 4mz  42600  4f  4f*5734  w0              -> RAM0[55..63]
;
; The normals (x y z, 2.14, 11 x 16: the last padded) come 16 at a time into
; RAM1, the weights go out 16 at a time from RAM2; after each job it adds one to
; a count in work RAM. The host sets RAM0[40..44]: the jobs' address >> 2, the
; normals', -, the count's address, 0; each model's header, its jobs (they come
; in the models' order).


ljob:   mov L_JOBS,ct0
        mov m0,ra0
        mov J_OUT,ct0
        dma d0,mc0,#9                   ; the job
w_lj:   jmp t0,w_lj
        nop
        mov L_JOBS,ct0
        mov m0,a
        mov 9,pl
        add  mov all,mc0                ; the next job's
        mov J_OUT,ct0
        mov m0,wa0                      ; its weights go here, 16 at a time
        mov L_NORM,ct0
        mov m0,ra0                      ; the normals from the start (RA0 moves on with each DMA)
        mov L_CHUNK,ct0
        mov 11,mc0
lchunk: mov 0,ct1
        dma d0,mc1,#48                  ; 16 normals
w_ln:   jmp t0,w_ln
        nop
        mov 0,ct1
        mov 0,ct2
        mov L_TOP,ct0
        mov m0,top
        mov 15,lop
        mov mc1,x    clr a    mov J_M,ct0     ; the first's x; A = 0; CT0 at 4m
el:     mov mc0,y                       ; RY = 4mx
        mov mul,p    mov mc1,x    mov mc0,y   ; P = x 4mx; RX = y; RY = 4my
        ad2  mov mul,p    mov mc1,x    mov mc0,y    mov alu,a  ; A = x 4mx; P = y 4my; RX = z; RY = 4mz
        ad2  mov mul,p    mov alu,a     ; A += y 4my; P = z 4mz
        ad2  mov alu,a    mov alh,rx    ; A += z 4mz: RX = dot (and the flags)
        jmp zs,wz                       ; not positive: w0
        mov mc0,y                       ; (either way) RY = 42600
        mov mul,p    clr a              ; P = dot 42600; A = 0
        ad2  mov alu,a    mov alh,rx    ; RX = t
        mov mc0,y                       ; RY = 4f
        mov mul,p    mov mc0,a          ; P = t 4f; A = 4f*5734
        ad2  mov alu,a    mov alh,mc2   ; the weight
tail:   btm
        mov mc1,x    clr a    mov J_M,ct0     ; the next's x; A = 0; CT0 at 4m again
        mov 0,ct2
        dma mc2,d0,#16                  ; 16 weights out
w_lo:   jmp t0,w_lo
        nop
        mov L_CHUNK,ct0
        mov m0,a
        mov 1,pl
        sub  mov all,mc0                ; a chunk fewer
        jmp nz,lchunk
        nop
        ; the job's done: count it, out where the CPUs can see
        mov L_COUNT,ct0
        mov m0,a
        mov 1,pl
        add  mov all,mc0
        mov L_CADDR,ct0
        mov m0,wa0
        mov L_COUNT,ct0
        dma mc0,d0,#1
w_lc:   jmp t0,w_lc
        nop
        mov L_LEFT,ct0
        mov m0,a
        mov 1,pl
        sub  mov all,mc0                ; a job fewer
        jmp nz,ljob
        nop
        jmp nextb                       ; the model's vertices
        nop

wz:     mov J_W0,ct0                    ; (not positive: w0)
        jmp tail
        mov m0,mc2
