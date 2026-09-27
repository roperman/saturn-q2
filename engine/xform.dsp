; SCU DSP batch vertex transform - double-buffered DMA
;
; Same maths and data formats as xform.dsp (see there), but DMA is hidden
; behind compute. Data RAM banks 1-3 rotate between three roles each block:
;
;   in     this block's vertices           (computed from)
;   out    this block's results            (computed into)
;   spare  DMA bank: first the previous block's results are written out of it,
;          then the next block's vertices are fetched into it
;
; so while block k is being transformed, block k-1 drains and block k+1 fills.
; Rotation (in, out, spare):  va = 1,2,3   vb = 3,1,2   vc = 2,3,1   -> va ...
;
; The inner loop is unrolled x2, so the loop branch is paid once per 2 vertices.
;
; RAM0[18] (block count) must be >= 1. It's counted down as blocks complete.
;
; Hardware note: a DSP DMA uses the bank's CT register as it runs, so we never
; touch a spare bank's CT until T0 (DMA busy) has cleared.

BLOCK   = 16
WORDS   = BLOCK*3
PARAMS  = 16
COUNT   = 18

; one matrix row over the 16 vertices of bank \in, results appended to \out
.macro rowpass in, out, base, ctin
        mov 0,\ctin
        mov \base,ct0
        mov \in,x    mov mc0,a          mov BLOCK/2-1,lop
        mov r\@,top
r\@:    mov mc0,y
        mov mul,p    mov \in,x    mov mc0,y
        ad2 mov mul,p   mov \in,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov alu,a    mov \base,ct0
        ad2 mov \in,x   mov mc0,a    mov all,\out
        mov mc0,y
        mov mul,p    mov \in,x    mov mc0,y
        ad2 mov mul,p   mov \in,x    mov mc0,y   mov alu,a
        ad2 mov mul,p   mov alu,a    mov \base,ct0
        btm
        ad2 mov \in,x   mov mc0,a    mov all,\out
.endm

; one block, for one rotation of the banks
.macro doblock name, in, out, spare, ctin, ctout, ctspare, next, entry
\name:  jmp t0,\name                    ; this block's input must have landed
        nop
        mov 0,\ctspare
        dma \spare,d0,#WORDS            ; drain previous block's results (async)
\entry: mov 0,\ctout
        rowpass \in, \out, 0, \ctin
        mov COUNT,ct0
        mov 1,pl
        mov m0,a
        sub  mov all,mc0                ; blocks left - 1; Z = this is the last one
        jmp z,rows\@
        nop
drain\@: jmp t0,drain\@                 ; results must be out before refilling
        nop
        mov 0,\ctspare
        dma d0,\spare,#WORDS            ; prefetch next block (async)
rows\@: rowpass \in, \out, 4, \ctin
        rowpass \in, \out, 8, \ctin
        mov COUNT,ct0
        mov 0,pl
        mov m0,a
        add                             ; Z = no blocks left
        jmp nz,\next
        nop
fin\@:  jmp t0,fin\@                    ; last block: flush its results, stop
        nop
        mov 0,\ctout
        dma \out,d0,#WORDS
done\@: jmp t0,done\@
        nop
        end
.endm

        mov PARAMS,ct0
        mov mc0,ra0
        mov mc0,wa0
        mov 0,ct1
        dma d0,mc1,#WORDS               ; first block, nothing to overlap with
first:  jmp t0,first
        nop
        jmp ea                          ; skip the drain: no previous results
        nop

        doblock va, mc1, mc2, mc3, ct1, ct2, ct3, vb, ea
        doblock vb, mc3, mc1, mc2, ct3, ct1, ct2, vc, eb
        doblock vc, mc2, mc3, mc1, ct2, ct3, ct1, va, ec
