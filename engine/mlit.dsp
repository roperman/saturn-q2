; SCU DSP: the models' dynamic lights, worked out a frame ahead (src/model.c ents_light_dyn,
; OPT=-DMLIGHT_DSP; OVERNIGHT.md 107). The models' program (xformm.dsp) loads this one over itself
; when its models are done, from the host's block in VDP1's VRAM (RAM0[56]: its address >> 2, the
; texture maker's slot), and this one ends by loading that back (RAM0[57]).
;
; The host's block: this program (256 words), the models' (256), then its words 512.. : the frame's tag (the slave
; writes it when the frame's jobs are listed), that frame's jobs >> 2, the tag again when this
; program's done (its own), the normals >> 2 (the 162, x y z 2.14 a word, padded to 176), the
; jobs. This program waits for the tag to match the master's (RAM0[58], set as it starts the
; models), for a while: given up, it loads the models' program back without a word written. The
; constants it uses (RAM0[41..54]) the host writes once a level, as the DSP's data RAM keeps them.
;
; A job (31 words): a word the host fills but this program doesn't read (where its table goes: see
; below), its entity's index, its lights (0..3), then each light's 7 words: kx ky
; kz (the direction to it in the model's space, 0.65 of 2.14 >> 6: at most 166), C = 5734 << 8,
; cr cg cb (the colour at the model, 0..124). For each normal n: w = C + max(n . k, 0) is 2^22
; times the weight 0.35 + 0.65 cos, and c w is 2^24 times what the light adds to that colour,
; which the C adds with a shift. The colours are summed as 2^24 a unit (the base's unpacked to
; that), each clamped to 31 at the end and packed 5-5-5 with bit 15 set, as the CPUs' tables are.
; A unit from the CPUs' at most.
;
; Nothing read by DMA is ever an address here, nor a count unmasked: on a Saturn a DMA read has
; come in a word late (OVERNIGHT.md 65, 66), and a job read wrong would have sent 88 words
; anywhere on the bus (108). The host sets, as it starts the models: RAM0[55] the frame's first
; table >> 2, RAM0[62] its first job >> 2; once a level, RAM0[39] the normals >> 2, RAM0[37] an
; entity's words (96) and RAM0[38] the first entity's base colours >> 2 (high work RAM: q_entity's
; gbase). Each job's table and job follow the last's; a job's second word is its entity's index,
; taken & 63 (the game's 64), its base colours that many entities on: a read inside them whatever
; was read. The head's count is taken & 15, a job's lights & 3: a word read wrong makes a frame's
; colours wrong, no more.
;
; Data RAM: RAM0 the block's words, the pointers, the job, the constants (41..54), the host's
; 56..58; RAM1 the chunk's 16 normals (48 words) then its 8 base words; RAM2 the chunk's 48 sums
; (r g b a normal); RAM3 the chunk's 8 words out, scratch, the 16 base colours split.
;
; JMP and BTM have a delay slot. An ALU result exists only in the instruction that makes it, so
; each op and its move share one.

B_FLAG  = 512                           ; the block's words after the two programs
B_DONE  = 514
P_FLAG  = 0                             ; (the block's 5 words, as read: tag, jobs, done, normals, how many)
P_JOB   = 1                             ; the next job's address
P_JOBS  = 4
P_OUT   = 6
P_BASE  = 7
P_NORM  = 8
P_CHUNK = 9
J_OUT   = 10                            ; the job
J_BASE  = 11
J_NL    = 12
J_L     = 13                            ; its lights, 7 words each
JOBW    = 31
LW      = 7
K_ENTW  = 37                            ; an entity's words, the first's base colours >> 2 (the host's, once a level)
K_ENTS  = 38
K_NORM  = 39                            ; the normals >> 2 (the host's, once a level)
K_255   = 41                            ; the constants: 255, 31, 2^24, 0x3E0, 2^19, 0x7C00, 2^14,
K_31    = 42                            ; 1, 32, 1024, 65536, 0x8000, the polls, 0xFFFF
K_1     = 48
K_65536 = 51
K_POLL  = 53
K_FFFF  = 54
K_OUT0  = 55                            ; the frame's first table >> 2 (the host's, as the models start)
W_MKP   = 56
W_P0    = 57
W_TAG   = 58
P_OI    = 59                            ; the word out's place in RAM3
P_N     = 60                            ; normals left (the unpack's, the pack's)
P_NL    = 61                            ; lights left
K_JOB0  = 62                            ; the frame's first job >> 2 (the host's)
N_BASE  = 48                            ; RAM1: the base words after the normals
T_OUT   = 0                             ; RAM3
T_W     = 8                             ; the word being packed
T_TOG   = 9                             ; which of a word's two colours
T_LB    = 10                            ; the light's words (RAM0)
T_PL    = 11                            ; the colour's place (RAM0)
T_PK    = 12                            ; the 16 base colours, split

start:  mov K_POLL,ct0
        mvi 12000,mc0                   ; polls before giving up (about 30 ms)
poll:   mov W_MKP,ct0
        mov m0,a
        mvi B_FLAG,pl
        add  mov all,ra0                ; the block's words after the program
        mov P_FLAG,ct0
        dma d0,mc0,#5
w_f:    jmp t0,w_f
        mov P_FLAG,ct0                  ; (the slot: as well each time round)
        mov m0,a
        mov W_TAG,ct0
        mov m0,p
        sub                             ; the frame's tag: the master's?
        jmp z,go
        mov K_POLL,ct0                  ; (the slot: go sets its own)
        mov m0,a
        mov 1,pl
        sub  mov all,mc0                ; not yet: again, for a while
        jmp nz,poll
        nop
        jmp back                        ; (given up: the models' program back, no word written)
        nop
go:     mov P_JOBS,ct0
        mov m0,a   mov 15,pl
        and  mov all,mc0                ; the jobs, 15 at most
        jmp z,fin
        mov K_JOB0,ct0                  ; (the slot: fin sets its own)
        mov m0,a   mov 0,pl
        mov P_JOB,ct0
        or   mov all,mc0                ; the first job
        mov K_OUT0,ct0
        mov m0,a
        mov P_OUT,ct0
        or   mov all,mc0                ; where its table goes (then on, 88 a job)
job:    mov P_JOB,ct0
        mov m0,ra0
        mov J_OUT,ct0
        dma d0,mc0,#24                  ; the job: its 3 words and 3 lights' 21 (RAM0[10..33]: 39, the
                                        ; normals' address, and 40, the walls' word, stay the host's)
w_j:    jmp t0,w_j
        mov P_JOB,ct0                   ; (the slot)
        mov m0,a
        mvi JOBW,pl
        add  mov all,mc0                ; the next's
        mov J_BASE,ct0
        mov m0,a   mov 63,pl
        and  mov all,rx                 ; X = its entity's index, inside the game's 64 whatever was read
        mov K_ENTW,ct0
        mov m0,y                        ; Y = an entity's words
        mov mul,p
        mov K_ENTS,ct0
        mov m0,a
        mov P_BASE,ct0
        add  mov all,mc0                ; its base colours (high work RAM)
        mov K_NORM,ct0
        mov m0,a   mov 0,pl
        mov P_NORM,ct0
        or   mov all,mc0                ; the normals, from the first
        mov P_CHUNK,ct0
        mov 11,mc0
chunk:  mov P_NORM,ct0
        mov m0,ra0
        mov 0,ct1
        dma d0,mc1,#48                  ; 16 normals
w_n1:   jmp t0,w_n1
        mov P_NORM,ct0                  ; (the slot)
        mov m0,a
        mov 48,pl
        add  mov all,mc0
        mov P_BASE,ct0
        mov m0,ra0
        mov N_BASE,ct1
        dma d0,mc1,#8                   ; their 16 base colours, two to a word
w_b:    jmp t0,w_b
        mov P_BASE,ct0                  ; (the slot)
        mov m0,a
        mov 8,pl
        add  mov all,mc0
        ; the base colours split (the high half the first normal's), then each unpacked into
        ; its three sums (RAM2), 2^24 a unit
        mov N_BASE,ct1
        mov T_PK,ct3
        mov K_FFFF,ct0
        mov m0,p
        mov 7,lop
        mov sp,top
sp:     mov mc1,a                       ; the word
        rl8  mov alu,a
        rl8  mov alu,a
        and  mov all,mc3                ; the high colour
        rl8  mov alu,a
        rl8  mov alu,a
        btm
        and  mov all,mc3                ; the low
        mov 0,ct2
        mov T_PK,ct3
        mov 15,lop
        mov up,top
up:     mov K_31,ct0
        mov m3,a
        mov mc0,p                       ; 31
        and  mov all,rx                 ; X = r
        mov mc0,y                       ; 2^24
        mov mul,p    clr a
        add  mov all,mc2   mov m3,a     ; r << 24; A = the colour again
        mov mc0,p                       ; 0x3E0
        and  mov all,rx                 ; X = g << 5
        mov mc0,y                       ; 2^19
        mov mul,p    clr a
        add  mov all,mc2   mov m3,a     ; g << 24
        mov mc0,p                       ; 0x7C00
        and  mov all,rx                 ; X = b << 10
        mov mc0,y                       ; 2^14
        mov mul,p    clr a
        btm
        add  mov all,mc2   mov mc3,a    ; b << 24 (and CT3 on to the next colour)
        ; each light's share added: w = C + max(n . k, 0), then c w a colour
        mov J_NL,ct0
        mov m0,a   mov 3,pl
        mov P_NL,ct0
        and  mov all,mc0                ; its lights, 3 at most
        jmp z,pack
        nop
        mov T_LB,ct3
        mov J_L,mc3                     ; the first light's words
light:  mov T_LB,ct3
        mov 0,ct1
        mov 0,ct2
        mov 15,lop
        mov el,top
        mov mc1,x    clr a    mov m3,ct0        ; X = x; A = 0; CT0 at kx
el:     mov mc0,y                               ; Y = kx
        mov mul,p    mov mc1,x    mov mc0,y     ; P = x kx; X = y; Y = ky
        ad2  mov mul,p    mov mc1,x    mov mc0,y    mov alu,a   ; A = x kx; P = y ky; X = z; Y = kz
        ad2  mov mul,p    mov alu,a             ; A += y ky; P = z kz
        ad2  mov alu,a    mov mc0,p             ; A = n . k; P = C
        jmp ns,pos
        mov mc0,y                               ; (either way) Y = cr
        clr a                                   ; facing away: 0
pos:    add  mov all,rx                         ; X = w
        mov mul,p    mov m2,a                   ; P = cr w; A = the red so far
        add  mov all,mc2   mov mc0,y            ; red; Y = cg
        mov mul,p    mov m2,a
        add  mov all,mc2   mov mc0,y            ; green; Y = cb
        mov mul,p    mov m2,a
        add  mov all,mc2                        ; blue
        btm
        mov mc1,x    clr a    mov m3,ct0        ; the next normal's x; A = 0; CT0 at kx again
        mov T_LB,ct3
        mov m3,a
        mov LW,pl
        add  mov all,mc3                        ; the next light's words
        mov P_NL,ct0
        mov m0,a
        mov 1,pl
        sub  mov all,mc0
        jmp nz,light
        nop
        ; the sums clamped and packed, two normals to a word: RAM3[T_OUT..]
pack:   mov 0,ct2
        mov P_OI,ct0
        mov T_OUT,mc0
        mov P_N,ct0
        mov 16,mc0
        mov T_W,ct3
        mov 0,mc3                               ; the word from nothing
        mov 0,mc3                               ; (T_TOG) its first colour next
pn:     mov T_W,ct3
        mov m3,x                                ; X = the word so far
        mov K_65536,ct0
        mov mc0,y                               ; Y = 65536
        mov mul,p    mov mc0,a                  ; P = the word << 16; A = 0x8000
        add  mov all,mc3                        ; this colour's bit 15, below the last's
        mov T_PL,ct3
        mov K_1-1,mc3                           ; the places, 1, 32, 1024: the first before the first
        mov 2,lop
        mov ln,top
ln:     mov T_PL,ct3
        mov m3,a   mov 1,pl
        add  mov all,mc3                        ; the next place
        mov K_255,ct0
        mov mc0,p    mov mc2,a                  ; P = 255; A = the sum (2^24 a unit)
        rl8  mov alu,a
        and  mov alu,a   mov m0,p               ; A = the units (0..255); P = 31
        sub                                     ; (A - 31: the flags)
        jmp s,ok
        mov 0,pl                                ; (either way) P = 0: A + P is A
        mvi 31,pl                               ; over: 0 + 31
        clr a
ok:     mov T_PL,ct3
        mov m3,ct0                              ; CT0 at the place
        add  mov all,rx   mov mc0,y             ; X = the colour; Y = its place
        mov T_W,ct3
        mov mul,p    mov m3,a                   ; P = colour x place; A = the word so far
        btm
        add  mov all,mc3                        ; the word
        mov T_TOG,ct3
        mov m3,a   mov 1,pl
        xor  mov all,mc3                        ; the word's first colour done: 1; its second: 0
        jmp nz,pnext
        nop
        mov T_W,ct3
        mov m3,a   mov 0,pl
        mov P_OI,ct0
        mov m0,ct3
        or   mov all,mc3                        ; the word out
        mov m0,a
        mov 1,pl
        add  mov all,mc0                        ; the next word's place
        mov T_W,ct3
        mov 0,mc3                               ; the next word from nothing
pnext:  mov P_N,ct0
        mov m0,a
        mov 1,pl
        sub  mov all,mc0                        ; a normal fewer
        jmp nz,pn
        mov P_OUT,ct0                   ; (the slot: pn sets its own)
        mov m0,wa0
        mov T_OUT,ct3
        dma mc3,d0,#8,1                         ; the 8 words out (the B-bus: add 1)
w_o:    jmp t0,w_o
        mov P_OUT,ct0                   ; (the slot)
        mov m0,a
        mov 8,pl
        add  mov all,mc0
        mov P_CHUNK,ct0
        mov m0,a
        mov 1,pl
        sub  mov all,mc0                        ; a chunk fewer
        jmp nz,chunk
        mov P_JOBS,ct0                  ; (the slot: chunk sets its own)
        mov m0,a
        mov 1,pl
        sub  mov all,mc0                        ; a job fewer
        jmp nz,job
        nop
fin:    mov W_MKP,ct0                           ; done: the tag, where the host looks next frame
        mov m0,a
        mvi B_DONE,pl
        add  mov all,wa0
        mov W_TAG,ct0
        dma mc0,d0,#1,1
w_d:    jmp t0,w_d
        nop
back:   mov W_P0,ct0                            ; the models' program back (it stops at its 255)
        mov m0,ra0
        jmp load
        nop

        .org 253
load:   dma d0,prg,256
        mvi 0,pc
        nop
