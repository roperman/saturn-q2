; SCU DSP: the hardware timing suite's programs (src/hwtest.c, OPT=-DHW_TEST). The host puts
; the address (>> 2) in RAM0[0] and, for the counted ones, how many 64-word DMAs in RAM0[1].
; The _loop ones DMA over and over till the host stops the DSP: they're the traffic the CPUs'
; timings are taken under. Reads land in RAM1; writes are RAM1's.

A_ADDR  = 0
A_N     = 1

rdloop: mov A_ADDR,ct0                  ; reads, for ever
        mov m0,ra0
        mov 0,ct1
        dma d0,mc1,64
w1:     jmp t0,w1
        nop
        jmp rdloop
        nop

wrloop: mov A_ADDR,ct0                  ; writes to work RAM, for ever
        mov m0,wa0
        mov 0,ct1
        dma mc1,d0,64
w2:     jmp t0,w2
        nop
        jmp wrloop
        nop

rdn:    mov A_ADDR,ct0                  ; RAM0[1] reads of 64 words
        mov m0,ra0
        mov 0,ct1
        dma d0,mc1,64
w3:     jmp t0,w3
        nop
        mov A_N,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0
        jmp nz,rdn
        nop
        end

wrn:    mov A_ADDR,ct0                  ; RAM0[1] writes of 64 words, to work RAM (add mode 2)
        mov m0,wa0
        mov 0,ct1
        dma mc1,d0,64
w4:     jmp t0,w4
        nop
        mov A_N,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0
        jmp nz,wrn
        nop
        end

wrbn:   mov A_ADDR,ct0                  ; ...to the B-bus (add mode 1: a word is two halves)
        mov m0,wa0
        mov 0,ct1
        dma mc1,d0,64,1
w5:     jmp t0,w5
        nop
        mov A_N,ct0
        mov m0,a  mov 1,pl
        sub  mov all,mc0
        jmp nz,wrbn
        nop
        end

; the same traffic in shorter bursts: does a CPU's miss wait for the rest of the DSP's burst?
rdloop16: mov A_ADDR,ct0                ; reads of 16 words, for ever
        mov m0,ra0
        mov 0,ct1
        dma d0,mc1,16
w6:     jmp t0,w6
        nop
        jmp rdloop16
        nop

wrloop16: mov A_ADDR,ct0                ; writes of 16 words to work RAM, for ever
        mov m0,wa0
        mov 0,ct1
        dma mc1,d0,16
w7:     jmp t0,w7
        nop
        jmp wrloop16
        nop

wrloop8: mov A_ADDR,ct0                 ; ...of 8
        mov m0,wa0
        mov 0,ct1
        dma mc1,d0,8
w8:     jmp t0,w8
        nop
        jmp wrloop8
        nop
