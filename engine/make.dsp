; SCU DSP: the textures made as they're uploaded (src/render.c tex_load: the level
; file holds a tile and a mask, not every cell's crop of it), after the models'
; job and the walls' lights, for the rest of the frame.
;
; Each CPU lists jobs on the cart as it finds such a texture to upload (its ring
; slot is the upload DMA's source, taken at the frame's end); this program polls
; the two lists and makes each into its slot. When the host says the frame's
; done it writes how many of each list it took (the host makes the rest itself)
; and loads the models' program back, which stops.
;
; The host's block starts with a copy of this program (RAM0[56]: its address
; >> 2, which walls2.dsp and xformm.dsp load it from; walls2.dsp has RAM0's
; 0-39 to itself), then: count0, count1, end, done0, done1, state (1 as this
; program starts, 2 as it finishes, with the dones: the host needn't wait for
; it if it never got going, nor for the models' program's reload after); the
; nibble table (16 words: nib4[i] << 16, bit b of i the texel b of four, the
; first the high nibble); list 0 (JOBS jobs of 12 words), list 1. A
; job, all worked out by the host: the tile's address >> 2 (16 x 16 texels,
; two to a byte, 32 words), the mask's >> 2 (16 rows of 16 bits, two rows a
; word, the even row the high half), the slot's >> 2, the kind (0 a crop, else
; the tile quartered), 0 (the crop's row: the mask's rows are the crop's), h
; (the end), the first row's first word in RAM1 (16 + 2 y0, + 1 for x0 = 8),
; the words a row (w / 8), whether windowed (w = 8
; and 0 < x0 < 8: the row's 8 texels from x0 are w0 << k | w1 >> (32 - k), k =
; 4 x0), k - 1 and 31 - k (the shifts' counts) and 2^k - 1 (the mask for the
; second: SR carries the sign down). A crop's mask row's bit x says texel x is
; in (0 if not); quartered: the four 8 x 8 quarters one under another, 4 bytes
; a row. Always 32 words out. (The transposed tile is the CPU's.)
;
; Data RAM: RAM0 the job, the counts, the host's; RAM1 the mask then the tile;
; RAM2 the nibble table and the row's words; RAM3 the words out.

J_T     = 0                             ; the job: tile, mask, slot, kind, ...
J_M     = 1
J_D     = 2
J_KIND  = 3
I_Y     = 4                             ; the crop's row (0 from the host), then
I_END_Y = 5                             ; h
I_SRC   = 6                             ; the row's first word in RAM1
I_WW    = 7                             ; words a row
I_WIN   = 8                             ; windowed
I_LSL   = 9                             ; k - 1
I_LSR   = 10                            ; 31 - k
I_M     = 11                            ; 2^k - 1
JOBW    = 12
I_CNT0  = 12                            ; count0 count1 end, as read
I_CNT1  = 13
I_END   = 14
I_DONE0 = 15                            ; jobs taken from each list, and the state (1 going, 2 done):
I_DONE1 = 16                            ; the host's, as it starts and as it finishes
I_STATE = 17
I_NEXT0 = 18                            ; the next job's address >> 2, each list
I_NEXT1 = 19
I_MROW  = 20                            ; the mask's row, the next word's bits lowest
I_WP    = 21                            ; the row's word (RAM2), then
I_WORDS = 22                            ; words left in it
W_MKP   = 56                            ; this program >> 2 (the host's, after it: + 256)
W_P0    = 57                            ; the models' program >> 2
B_CNT   = 256                           ; the block's words from the program: the counts, end,
B_DONE  = 259                           ; done0 done1 state,
B_TAB   = 262                           ; the table,
B_LIST  = 278                           ; list 0
TILE    = 16                            ; RAM1: the mask 0..7, the tile 16..47
S_W0    = 16                            ; RAM2: the table 0..15, the row's words, the window's shifted first
S_W1    = 17
S_T     = 18
S_A     = 19                            ; (a word's first nibble masks)
JOBS    = 64

make:   mov W_MKP,ct0
        mov m0,a
        mvi B_TAB,pl
        add  mov all,ra0                ; the table
        mov 0,ct2
        dma d0,mc2,16
w0:     jmp t0,w0
        nop
        mvi B_LIST,pl
        mov I_NEXT0,ct0
        add  mov all,mc0                ; list 0
        mvi B_LIST+JOBS*JOBW,pl
        add  mov all,mc0                ; list 1 after it
        mov I_DONE0,ct0
        mov 0,mc0
        mov 0,mc0
        mov 1,mc0                       ; going
        mvi B_DONE,pl
        add  mov all,wa0
        mov I_DONE0,ct0
        dma mc0,d0,3
wr:     jmp t0,wr
        nop

poll:   mov W_MKP,ct0
        mov m0,a
        mvi B_CNT,pl
        add  mov all,ra0
        mov I_CNT0,ct0
        dma d0,mc0,3                    ; the counts and the end
w1:     jmp t0,w1
        nop
        mov I_END,ct0
        mov m0,a  mov 0,pl
        or
        jmp nz,fin                      ; the frame's done
        mov I_CNT0,ct0
        mov m0,a  mov I_DONE0,ct0
        mov m0,p
        sub                             ; count0 - done0
        jmp zs,try1                     ; none new on list 0
        mov I_NEXT0,ct0
        mov m0,ra0                      ; its next job
        mov m0,a  mov JOBW,pl
        add  mov all,mc0                ; (the one after)
        mov I_DONE0,ct0
        mov m0,a  mov 1,pl
        jmp job
        add  mov all,mc0
try1:   mov I_CNT1,ct0
        mov m0,a  mov I_DONE1,ct0
        mov m0,p
        sub
        jmp zs,idle
        mov I_NEXT1,ct0
        mov m0,ra0
        mov m0,a  mov JOBW,pl
        add  mov all,mc0
        mov I_DONE1,ct0
        mov m0,a  mov 1,pl
        jmp job
        add  mov all,mc0
idle:   mvi 199,lop                     ; (a pause between polls: the cart's bus is the CPUs' too)
        lps
        nop
        jmp poll
        nop

job:    mov J_T,ct0
        dma d0,mc0,JOBW                 ; the job
w2:     jmp t0,w2
        nop
        mov J_T,ct0
        mov m0,ra0
        mov TILE,ct1
        dma d0,mc1,32                   ; the tile
w3:     jmp t0,w3
        nop
        mov J_KIND,ct0
        mov m0,a  mov 0,pl
        or
        jmp nz,quart
        mov 0,ct3                       ; (the words out, from RAM3's start)

        ; ---- a masked crop ----
        mov J_M,ct0
        mov m0,ra0
        mov 0,ct1
        dma d0,mc1,8                    ; the mask
w4:     jmp t0,w4
        nop
row:    ; the mask's row: word y / 2, the high half for an even y (SR's carry: y odd)
        mov I_Y,ct0
        mov m0,a
        sr   mov all,ct1
        jmp c,odd
        mov m1,a                        ; (the word)
        rl8  mov alu,a
        rl8  mov alu,a                  ; the high half down
odd:    mvi $FFFF,pl
        mov I_MROW,ct0
        and  mov all,mc0
        ; its words from the tile, to RAM2: both (one's enough when w is 8 and x0 is 0 or 8:
        ; I_SRC is the one's); windowed, the window over the first
        mov I_SRC,ct0
        mov m0,ct1
        mov m0,a  mov 2,pl
        add  mov all,mc0                ; (the next row's)
        mov S_W0,ct2
        mov mc1,mc2
        mov mc1,mc2
        mov m0,a  mov I_WP,ct0          ; (I_WW)
        mov S_W0,mc0                    ; the first word
        mov all,mc0                     ; the words
        mov I_WIN,ct0
        mov m0,a  mov 0,pl
        or
        jmp z,word
        mov I_MROW,ct0
        ; the window: w0 << k | (w1 >> (32 - k)) & M
        mov S_W0,ct2
        mov mc2,a                       ; w0
        mov I_LSL,ct0
        mov m0,lop
        lps
        sl   mov alu,a
        mov S_T,ct2
        mov all,mc2                     ; w0 << k
        mov S_W1,ct2
        mov m2,a                        ; w1
        mov I_LSR,ct0
        mov m0,lop
        lps
        sr   mov alu,a
        mov I_M,ct0
        mov m0,p  mov S_T,ct2
        and  mov alu,a                  ; (w1 >> (32 - k)) & M
        mov m2,p  mov S_W0,ct2
        or   mov all,mc2                ; the window, as the row's first word
        mov I_MROW,ct0

        ; a word: the table for its first four texels' bits, for the next four (the same entry's
        ; high half: ALH), ORed, AND the word, out; the row's bits on by 8
word:   mov m0,a  mov 15,pl
        and  mov all,ct2                ; the first four's entry
        mov m2,a  mov S_A,ct2
        mov m0,a  mov all,mc2           ; (kept in S_A)
        sr   mov alu,a
        sr   mov alu,a
        sr   mov alu,a
        sr   mov alu,a
        and  mov all,ct2                ; the next four's
        sr   mov alu,a
        sr   mov alu,a
        sr   mov alu,a
        sr   mov alu,a
        mov m2,p  clr a  mov all,mc0    ; the bits on by 8, for the next word; their entry
        or   mov alu,a                  ; (from nothing: a 32-bit load's sign fills ALH)
        mov alh,pl                      ; the next four's masks: its high half
        mov S_A,ct2
        mov m2,a  mov m0,ct2            ; the first four's; the word
        or   mov alu,a
        mov m2,p
        and  mov all,mc3                ; the word masked, out
        mov m0,a  mov 1,pl
        add  mov all,mc0                ; the next word
        mov m0,a
        sub  mov all,mc0                ; one fewer
        jmp nz,word
        mov I_MROW,ct0
        ; the next row, while there are rows
        mov I_Y,ct0
        mov m0,a  mov 1,pl
        add  mov alu,a
        mov all,mc0
        mov m0,p                        ; the end
        sub
        jmp s,row
        nop

out:    mov J_D,ct0
        mov m0,wa0
        mov 0,ct3
        dma mc3,d0,32                   ; the 32 words out, to its slot
w5:     jmp t0,w5
        nop
        jmp poll
        nop

quart:  ; ---- the tile quartered: out[q * 8 + r] = tile[((q / 2) * 8 + r) * 2 + (q & 1)] ----
        mov TILE,ct1                    ; top left: words 0, 2, .. 14
        mov 7,lop
        mov q0,top
q0:     mov mc1,mc3
        btm
        mov mc1,a
        mov TILE+1,ct1                  ; top right: 1, 3, .. 15
        mov 7,lop
        mov q1,top
q1:     mov mc1,mc3
        btm
        mov mc1,a
        mov TILE+16,ct1                 ; bottom left: 16, 18, .. 30
        mov 7,lop
        mov q2,top
q2:     mov mc1,mc3
        btm
        mov mc1,a
        mov TILE+17,ct1                 ; bottom right
        mov 7,lop
        mov q3,top
q3:     mov mc1,mc3
        btm
        mov mc1,a
        jmp out
        nop

fin:    mov I_STATE,ct0                 ; done: how many of each list were made, to the host
        mov 2,mc0
        mov W_MKP,ct0
        mov m0,a
        mvi B_DONE,pl
        add  mov all,wa0
        mov I_DONE0,ct0
        dma mc0,d0,3
w6:     jmp t0,w6
        nop
        mov W_P0,ct0                    ; the models' program back (it stops at its 255)
        mov m0,ra0
        jmp load
        nop

        .org 253
load:   dma d0,prg,256
        mvi 0,pc
        nop
