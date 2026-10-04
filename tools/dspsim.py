#!/usr/bin/env python3
"""SCU DSP simulator, for testing our DSP programs off the Saturn (tools/walls_sim.py uses it).

Follows Mednafen's DSP (src/ss/scu_dsp_*.cpp, scu.inc), which our test harness runs on, as far as our
programs need it: the ALU (AD2's 48 bits, the rest's 32), the X/Y/D1 buses and their counters (a bank
read by an instruction isn't written by it), the prefetch (JMP and BTM's delay slot), LPS, BTM, MVI
(conditional: nothing if not), DMA (done at once: its words at the instruction; a program's own words
into program RAM when the MVI to PC after it runs), END. Not: timing (cycles here are a rough count),
T0 (DMA's done at once), the interrupts, the host's port.

    mem = Mem(); mem.add(0x06000000, data)         # external memory: regions, big-endian
    d = DSP(mem); d.prog = load_bin("obj/walls0.bin")
    d.ram[0][40] = ...                              # data RAM, as the host would set it
    d.start(0); d.run()
"""
import struct

M48 = (1 << 48) - 1
M32 = 0xFFFFFFFF


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


class Mem:
    """big-endian external memory: regions of bytearrays"""
    def __init__(self):
        self.regions = []

    def add(self, base, data):
        self.regions.append((base & 0x07FFFFFF, bytearray(data)))

    def find(self, addr):
        addr &= 0x07FFFFFF
        for base, b in self.regions:
            if base <= addr < base + len(b):
                return b, addr - base
        raise KeyError(hex(addr))

    def r32(self, addr):
        b, o = self.find(addr)
        return struct.unpack_from(">I", b, o)[0]

    def w32(self, addr, v):
        b, o = self.find(addr)
        struct.pack_into(">I", b, o, v & M32)

    def w16(self, addr, v):
        b, o = self.find(addr)
        struct.pack_into(">H", b, o, v & 0xFFFF)


def bbus(addr):
    """the B-bus (VDP1, VDP2, the SCSP): 16 bits wide"""
    addr &= 0x07FFFFFF
    return 0x05A00000 <= addr < 0x05FE0000


class DSP:
    def __init__(self, mem):
        self.mem = mem
        self.prog = [0] * 256
        self.ram = [[0] * 64 for _ in range(4)]
        self.ct = [0] * 4
        self.rx = self.ry = 0
        self.p = 0          # int (64-bit, as Mednafen's P.T)
        self.ac = 0         # int (Mednafen's AC.T: 64-bit)
        self.z = self.s = self.c = self.v = False
        self.ra0 = self.wa0 = 0
        self.lop = 0
        self.top = 0
        self.pc = 0
        self.next = (0, False)
        self.ended = False
        self.pram = None
        self.cycles = 0
        self.trace = None

    def start(self, pc):
        self.pc = pc
        self.next = (self.prog[pc], False)
        self.pc = (pc + 1) & 0xFF
        self.ended = False

    def cond(self, c):
        if not (c & 0x40):
            return True
        r = False
        if c & 1: r |= self.z
        if c & 2: r |= self.s
        if c & 4: r |= self.c
        if c & 8: r |= False            # T0: DMA is instant here
        return r == bool(c & 0x20)

    def step(self):
        instr, looped = self.next
        at = (self.pc - 1) & 0xFF
        if not looped or not self.lop:
            self.next = (self.prog[self.pc], False)
            self.pc = (self.pc + 1) & 0xFF
        if looped:
            self.lop = (self.lop - 1) & 0xFFF
        self.cycles += 1
        if self.trace:
            self.trace(self, at, instr)
        t = (instr >> 28) & 0xF
        if t <= 3:
            self.general(instr, looped)
        elif 8 <= t <= 0xB:
            self.mvi(instr, looped)
        elif t == 0xC:
            self.dma(instr)
        elif t == 0xD:
            if self.cond((instr >> 19) & 0x7F):
                self.pc = instr & 0xFF
        else:
            op = (instr >> 27) & 3
            if op in (2, 3):
                if self.pram is not None:
                    self.finish_pram()
                else:
                    self.ended = True
            elif op == 0:           # BTM
                if self.lop:
                    self.pc = self.top
                self.lop = (self.lop - 1) & 0xFFF
            elif op == 1:           # LPS
                self.next = (self.next[0], True)

    def finish_pram(self):
        for i, w in enumerate(self.pram):
            self.prog[(self.pc + i) & 0xFF] = w
        self.pram = None
        self.pc = self.top
        self.next = (0, False)

    def mvi(self, instr, looped):
        dest = (instr >> 26) & 0xF
        cond = (instr >> 19) & 0x7F
        imm = sx(instr, 19) if cond & 0x40 else sx(instr, 25)
        if not self.cond(cond):
            return
        if dest <= 3:
            self.ram[dest][self.ct[dest]] = imm & M32
            self.ct[dest] = (self.ct[dest] + 1) & 0x3F
        elif dest == 4: self.rx = imm & M32
        elif dest == 5: self.p = imm
        elif dest == 6: self.ra0 = imm & M32
        elif dest == 7: self.wa0 = imm & M32
        elif dest == 0xA:
            if not looped or self.lop == 0xFFF:
                self.lop = imm & 0xFFF
        elif dest == 0xC:
            self.top = (self.pc - 1) & 0xFF
            self.pc = imm & 0xFF
            if self.pram is not None:
                self.finish_pram()
        else:
            raise ValueError("mvi dest %x" % dest)

    def dma(self, instr):
        add_mode = (instr >> 15) & 7
        hold = (instr >> 14) & 1
        fmt = (instr >> 13) & 1
        d = (instr >> 12) & 1
        drw = (instr >> 8) & 7
        if fmt:
            crw = instr & 3
            count = self.ram[crw][self.ct[crw]] & 0xFF
            self.ct[crw] = (self.ct[crw] + ((instr >> 2) & 1)) & 0x3F
        else:
            count = instr & 0xFF
        if count == 0:
            count = 256
        if d:
            addr = (self.wa0 << 2) & 0x07FFFFFF
            step = (1 << add_mode) & ~1
            for _ in range(count):
                v = self.ram[drw][self.ct[drw]]
                self.ct[drw] = (self.ct[drw] + 1) & 0x3F
                if bbus(addr):
                    self.mem.w16(addr, v >> 16)
                    self.mem.w16(addr + step, v)
                    addr += 2 * step
                else:
                    self.mem.w32(addr & ~3, v)
                    addr += step
            if not hold:
                self.wa0 = (addr + 2) >> 2
        else:
            addr = (self.ra0 << 2) & 0x07FFFFFF
            step = 4 if bbus(addr) else (1 << (add_mode & 2)) & ~1
            if drw & 4:
                self.pram = []
            for _ in range(count):
                v = self.mem.r32(addr)
                addr += step
                if drw & 4:
                    self.pram.append(v)
                else:
                    self.ram[drw][self.ct[drw]] = v
                    self.ct[drw] = (self.ct[drw] + 1) & 0x3F
            if not hold:
                self.ra0 = addr >> 2
        self.cycles += count * 8        # (a rough A-bus cost)

    def general(self, instr, looped):
        alu_op = (instr >> 26) & 0xF
        x_op = (instr >> 23) & 7
        y_op = (instr >> 17) & 7
        d1_op = (instr >> 12) & 3
        ALU = self.ac & ((1 << 64) - 1)
        L = ALU & M32
        hi = ALU & ~M32
        dr_read = 0
        ct_inc = [0, 0, 0, 0]
        pl = self.p & M32

        def zs32(v):
            self.s = bool(v & 0x80000000)
            self.z = (v & M32) == 0

        if alu_op == 1:
            L = L & pl; self.c = False; zs32(L); ALU = hi | L
        elif alu_op == 2:
            L = L | pl; self.c = False; zs32(L); ALU = hi | L
        elif alu_op == 3:
            L = L ^ pl; self.c = False; zs32(L); ALU = hi | L
        elif alu_op == 4:
            t = L + pl; self.c = bool(t >> 32); zs32(t); ALU = hi | (t & M32)
        elif alu_op == 5:
            t = (L - pl) & ((1 << 64) - 1); self.c = bool((t >> 32) & 1); zs32(t); ALU = hi | (t & M32)
        elif alu_op == 6:
            t = (ALU & M48) + (self.p & M48)
            self.c = bool((t >> 48) & 1)
            self.s = bool((t >> 47) & 1)
            self.z = (t & M48) == 0
            ALU = t
        elif alu_op == 8:
            self.c = bool(L & 1); L = (s32(L) >> 1) & M32; zs32(L); ALU = hi | L
        elif alu_op == 9:
            c = L & 1; self.c = bool(c); L = (L >> 1) | (c << 31); zs32(L); ALU = hi | L
        elif alu_op == 0xA:
            self.c = bool(L >> 31); L = (L << 1) & M32; zs32(L); ALU = hi | L
        elif alu_op == 0xB:
            c = L >> 31; self.c = bool(c); L = ((L << 1) | c) & M32; zs32(L); ALU = hi | L
        elif alu_op == 0xF:
            self.c = bool((L >> 24) & 1); L = ((L << 8) | (L >> 24)) & M32; zs32(L); ALU = hi | L
        elif alu_op != 0:
            raise ValueError("alu %x" % alu_op)

        # X
        if (x_op & 3) == 2:
            self.p = s32(self.rx) * s32(self.ry)
        if x_op >= 3:
            s = (instr >> 20) & 7
            b = s & 3
            v = self.ram[b][self.ct[b]]
            dr_read |= 1 << b
            if s & 4: ct_inc[b] = 1
            if (x_op & 3) == 3:
                self.p = s32(v)
            if x_op & 4:
                self.rx = v
        # Y
        if (y_op & 3) == 1:
            self.ac = 0
        elif (y_op & 3) == 2:
            self.ac = ALU
        if y_op >= 3:
            s = (instr >> 14) & 7
            b = s & 3
            v = self.ram[b][self.ct[b]]
            dr_read |= 1 << b
            if s & 4: ct_inc[b] = 1
            if (y_op & 3) == 3:
                self.ac = s32(v) & ((1 << 64) - 1)
            if y_op & 4:
                self.ry = v
        # D1
        if d1_op & 1:
            d = (instr >> 8) & 0xF
            src = sx(instr, 8) & M32
            if d1_op & 2:
                sidx = instr & 0xF
                if sidx in (8, 0xB, 0xC, 0xD, 0xE, 0xF):
                    src = M32
                elif sidx <= 3:
                    src = self.ram[sidx][self.ct[sidx]]; dr_read |= 1 << sidx
                elif sidx <= 7:
                    b = sidx & 3
                    src = self.ram[b][self.ct[b]]; dr_read |= 1 << b
                    if d != b: ct_inc[b] = 1
                elif sidx == 9:
                    src = ALU & M32
                elif sidx == 0xA:
                    src = (ALU >> 16) & M32
            if d <= 3:
                if not (dr_read & (1 << d)):
                    self.ram[d][self.ct[d]] = src
                    ct_inc[d] = 1
            elif d == 4: self.rx = src
            elif d == 5: self.p = s32(src)
            elif d == 6: self.ra0 = src
            elif d == 7: self.wa0 = src
            elif d == 0xA:
                if not looped or self.lop == 0xFFF:
                    self.lop = src & 0xFFF
            elif d == 0xB: self.top = src & 0xFF
            elif d >= 0xC:
                self.ct[d - 0xC] = src & 0x3F
                ct_inc[d - 0xC] = 0
        for b in range(4):
            self.ct[b] = (self.ct[b] + ct_inc[b]) & 0x3F

    def run(self, limit=10_000_000):
        n = 0
        while not self.ended and n < limit:
            self.step()
            n += 1
        return n


def load_bin(path):
    b = open(path, "rb").read()
    return [struct.unpack_from(">I", b, 4 * i)[0] for i in range(len(b) // 4)]
