#!/usr/bin/env python3
r"""Tiny SCU DSP assembler.

Encodings follow Mednafen's SCU DSP implementation (src/ss/scu_dsp_*.cpp),
which is what our test harness runs on and is hardware-verified.

Syntax: one DSP instruction per line; the parallel slots of a general
instruction are just written side by side:

    loop:   AD2  MOV MUL,P  MOV MC1,X  MOV MC0,Y  MOV ALU,A   ; comment
            MOV 5,CT0                 ; D1 bus immediate (int8)
            MVI 1234,LOP              ; 25-bit immediate (19-bit if conditional)
            MVI 12,PC,Z               ; conditional MVI
            DMA D0,MC1,48             ; external (RA0) -> data RAM 1, 48 words
            DMA MC2,D0,48             ; data RAM 2 -> external (WA0)
            DMA MC3,D0,32,1           ; ...with add mode 1 (default 2: +4 a word; the B-bus wants 1)
            DMAH ...                  ; same, but don't advance RA0/WA0
            JMP T0,wait               ; conditions: Z NZ S NS C NC T0 NT0 ZS NZS
            BTM / LPS / END / ENDI / NOP

Remember: JMP and BTM have one delay slot (the next instruction always runs).

".org N": NOPs up to address N (a program's loader at the end of program RAM).

Macros (our extension, not in Sega's assembler):

    .macro rowpass in, out, base
    loop\@:  mov \in,x ...          ; \name = argument, \@ = unique expansion number
    .endm
            rowpass mc1, mc2, 0

usage: dspasm.py input.dsp output.h symbol_name [output.bin]
"""
import re
import sys

ALU = {"NOP": 0x0, "AND": 0x1, "OR": 0x2, "XOR": 0x3, "ADD": 0x4, "SUB": 0x5, "AD2": 0x6,
       "SR": 0x8, "RR": 0x9, "SL": 0xA, "RL": 0xB, "RL8": 0xF}
BUS_SRC = {"M0": 0, "M1": 1, "M2": 2, "M3": 3, "MC0": 4, "MC1": 5, "MC2": 6, "MC3": 7}
D1_SRC = dict(BUS_SRC, ALL=0x9, ALH=0xA)
D1_DST = {"MC0": 0x0, "MC1": 0x1, "MC2": 0x2, "MC3": 0x3, "RX": 0x4, "PL": 0x5, "RA0": 0x6, "WA0": 0x7,
          "LOP": 0xA, "TOP": 0xB, "CT0": 0xC, "CT1": 0xD, "CT2": 0xE, "CT3": 0xF}
MVI_DST = {"MC0": 0x0, "MC1": 0x1, "MC2": 0x2, "MC3": 0x3, "RX": 0x4, "PL": 0x5, "RA0": 0x6, "WA0": 0x7,
           "LOP": 0xA, "PC": 0xC}
COND = {"Z": 0x61, "NZ": 0x41, "S": 0x62, "NS": 0x42, "C": 0x64, "NC": 0x44,
        "T0": 0x68, "NT0": 0x48, "ZS": 0x63, "NZS": 0x43}
DMA_RAM = {"MC0": 0, "MC1": 1, "MC2": 2, "MC3": 3, "PRG": 4}


class AsmError(Exception):
    pass


def num(tok, labels):
    """Number, label or simple expression. Sega syntax: $hex, %binary, #immediate."""
    expr = tok.lstrip("#")
    expr = re.sub(r"\$([0-9A-Fa-f]+)", lambda m: str(int(m.group(1), 16)), expr)
    expr = re.sub(r"%([01]+)", lambda m: str(int(m.group(1), 2)), expr)
    expr = re.sub(r"[A-Za-z_]\w*", lambda m: str(labels[m.group(0).lower()]) if m.group(0).lower() in labels
                  else m.group(0), expr)
    if not re.fullmatch(r"[0-9xXa-fA-F+\-*/%~&|^<>() ]+", expr):
        raise AsmError(f"bad number or unknown label '{tok}'")
    try:
        return int(eval(expr.replace("/", "//"), {"__builtins__": {}}))
    except Exception:
        raise AsmError(f"bad expression '{tok}'")


def general(tokens, labels):
    """Encode a general (ALU / X / Y / D1) instruction from its slot tokens."""
    alu = None
    x_ops, x_src = 0, None
    y_ops, y_src = 0, None
    d1 = None
    i = 0

    def set_src(cur, s, bus):
        if cur is not None and cur != s:
            raise AsmError(f"{bus}-bus can only read one source per instruction")
        return s

    while i < len(tokens):
        t = tokens[i].upper()
        if t in ALU:
            if alu is not None:
                raise AsmError("two ALU ops in one instruction")
            alu = ALU[t]
            i += 1
            continue
        if t == "CLR":
            if i + 1 >= len(tokens) or tokens[i + 1].upper() != "A":
                raise AsmError("expected CLR A")
            y_ops |= 0x1
            i += 2
            continue
        if t != "MOV":
            raise AsmError(f"unknown op '{tokens[i]}'")
        if i + 1 >= len(tokens):
            raise AsmError("MOV without operands")
        ops = tokens[i + 1].split(",")
        if len(ops) != 2:
            raise AsmError(f"bad MOV operands '{tokens[i + 1]}'")
        s, d = ops[0].upper(), ops[1].upper()
        i += 2
        if d == "X":
            if s not in BUS_SRC:
                raise AsmError("MOV [s],X needs M0-3/MC0-3")
            x_src = set_src(x_src, BUS_SRC[s], "X")
            x_ops |= 0x4
        elif d == "P" and s == "MUL":
            x_ops |= 0x2
        elif d == "P":
            if s not in BUS_SRC:
                raise AsmError("MOV [s],P needs M0-3/MC0-3")
            x_src = set_src(x_src, BUS_SRC[s], "X")
            x_ops |= 0x3
        elif d == "Y":
            if s not in BUS_SRC:
                raise AsmError("MOV [s],Y needs M0-3/MC0-3")
            y_src = set_src(y_src, BUS_SRC[s], "Y")
            y_ops |= 0x4
        elif d == "A" and s == "ALU":
            y_ops |= 0x2
        elif d == "A":
            if s not in BUS_SRC:
                raise AsmError("MOV [s],A needs M0-3/MC0-3")
            y_src = set_src(y_src, BUS_SRC[s], "Y")
            y_ops |= 0x3
        elif d in D1_DST:
            if d1 is not None:
                raise AsmError("two D1-bus moves in one instruction")
            if s in D1_SRC:
                d1 = (0x3 << 12) | (D1_DST[d] << 8) | D1_SRC[s]
            else:
                v = num(s, labels)
                if not -128 <= v <= 255:
                    raise AsmError(f"D1 immediate {v} out of 8-bit range (use MVI)")
                d1 = (0x1 << 12) | (D1_DST[d] << 8) | (v & 0xFF)
        else:
            raise AsmError(f"unsupported MOV {s},{d}")

    # CLR A (01) | MOV ALU,A (10) would merge into MOV [s],A (11): reject
    if (y_ops & 0x3) == 0x3 and y_src is None:
        raise AsmError("CLR A and MOV ALU,A can't share an instruction")
    word = ((alu or 0) << 26) | (x_ops << 23) | ((x_src or 0) << 20) | (y_ops << 17) | ((y_src or 0) << 14)
    if d1 is not None:
        word |= d1
    return word


def encode(line, labels):
    line = re.sub(r"\s*,\s*", ",", line)
    toks = line.split()
    op = toks[0].upper()
    args = toks[1].split(",") if len(toks) > 1 else []
    if op in ("BTM", "LPS", "END", "ENDI"):
        return {"BTM": 0xE0000000, "LPS": 0xE8000000, "END": 0xF0000000, "ENDI": 0xF8000000}[op]
    if op == "JMP":
        if len(args) == 1:
            return 0xD0000000 | (num(args[0], labels) & 0xFF)
        cond = COND[args[0].upper()]
        return 0xD0000000 | (cond << 19) | (num(args[1], labels) & 0xFF)
    if op == "MVI":
        imm, dst = num(args[0], labels), MVI_DST[args[1].upper()]
        w = 0x80000000 | (dst << 26)
        if len(args) == 3:
            if not -(1 << 18) <= imm < (1 << 18):
                raise AsmError("conditional MVI immediate is 19-bit")
            return w | (COND[args[2].upper()] << 19) | (imm & 0x7FFFF)
        if not -(1 << 24) <= imm < (1 << 24):
            raise AsmError("MVI immediate is 25-bit")
        return w | (imm & 0x1FFFFFF)
    if op in ("DMA", "DMAH"):
        hold = 1 if op == "DMAH" else 0
        src, dst, cnt = (a.upper() for a in args[:3])
        add_mode = num(args[3], labels) if len(args) > 3 else 2     # (1: the B-bus, +2 a half)
        if not 0 <= add_mode <= 7:
            raise AsmError("DMA add mode 0..7")
        if src == "D0":
            direction, ram = 0, DMA_RAM[dst]     # external -> DSP
        elif dst == "D0":
            direction, ram = 1, DMA_RAM[src]     # DSP -> external
        else:
            raise AsmError("DMA needs D0 on one side")
        w = 0xC0000000 | (add_mode << 15) | (hold << 14) | (direction << 12) | (ram << 8)
        if cnt in D1_SRC and D1_SRC[cnt] < 8:
            # count taken from data RAM (format bit)
            s = D1_SRC[cnt]
            return w | (1 << 13) | (s & 0x3) | ((s & 0x4))
        n = num(cnt, labels)
        if not 1 <= n <= 256:
            raise AsmError("DMA count 1..256")
        return w | (n & 0xFF)
    return general(toks, labels)


def expand_macros(src):
    """Textual macros: .macro name a, b / ... / .endm, then 'name x, y' expands.
    Macros may invoke other macros; \\@ is unique per expansion."""
    macros, top, cur = {}, [], None
    for raw in src.splitlines():
        line = raw.split(";")[0].strip()
        m = re.match(r"^\.macro\s+(\w+)\s*(.*)$", line, re.I)
        if m:
            cur = (m.group(1).lower(), [a.strip().lower() for a in m.group(2).split(",") if a.strip()], [])
            continue
        if re.match(r"^\.endm\b", line, re.I):
            macros[cur[0]] = cur
            cur = None
            continue
        if cur is not None:
            cur[2].append(raw)
        else:
            top.append(raw)
    if cur is not None:
        raise AsmError(f"unterminated .macro {cur[0]}")

    count = [0]

    def expand(lines, depth):
        if depth > 16:
            raise AsmError("macro nesting too deep")
        out = []
        for raw in lines:
            line = raw.split(";")[0].strip()
            m = re.match(r"^(?:([A-Za-z_]\w*):\s*)?(\w+)\s*(.*)$", line)
            if not (m and m.group(2).lower() in macros):
                out.append(raw)
                continue
            name, params, body = macros[m.group(2).lower()]
            args = [a.strip() for a in m.group(3).split(",")] if m.group(3).strip() else []
            if len(args) != len(params):
                raise AsmError(f"macro {name} takes {len(params)} args: {raw.strip()}")
            count[0] += 1
            n = count[0]
            if m.group(1):
                out.append(m.group(1) + ":")
            sub = []
            for b in body:
                for p_, a in zip(params, args):
                    b = re.sub(r"\\" + p_ + r"\b", lambda _m, a=a: a, b, flags=re.I)
                sub.append(b.replace("\\@", str(n)))
            out.extend(expand(sub, depth + 1))
        return out

    return "\n".join(expand(top, 0))


def assemble(src):
    src = expand_macros(src)
    lines = []
    labels = {}
    for n, raw in enumerate(src.splitlines(), 1):
        line = raw.split(";")[0].strip()
        if line.upper().startswith("ENDS"):
            break
        m = re.match(r"^([A-Za-z_]\w*)\s*(?:=|\s+EQU\s)\s*(.+)$", line, re.I)
        if m:
            labels[m.group(1).lower()] = num(m.group(2).strip(), labels)
            continue
        if line.upper().startswith("ORG"):
            continue
        m = re.match(r"^\.org\s+(.+)$", line, re.I)
        if m:
            lines.append((n, raw, None, ".org " + m.group(1)))
            continue
        label = None
        m = re.match(r"^([A-Za-z_]\w*):\s*(.*)$", line)
        if m:
            label, line = m.group(1).lower(), m.group(2).strip()
        lines.append((n, raw, label, line))
    pc = 0
    for n, raw, label, line in lines:
        if label:
            if label in labels:
                raise AsmError(f"line {n}: duplicate label {label}")
            labels[label] = pc
        if line.startswith(".org "):
            at = num(line[5:].strip(), labels)
            if at < pc:
                raise AsmError(f"line {n}: .org {at}, but the program's already at {pc}")
            pc = at
        elif line:
            pc += 1
    if pc > 256:
        raise AsmError(f"program is {pc} words; program RAM holds 256")
    out, listing = [], []
    for n, raw, label, line in lines:
        if line.startswith(".org "):
            at = num(line[5:].strip(), labels)
            while len(out) < at:
                out.append(0)                   # (NOPs up to there)
            listing.append(f"             {raw}")
            continue
        if not line:
            listing.append(f"             {raw}")
            continue
        try:
            w = encode(line, labels)
        except (AsmError, KeyError, IndexError) as e:
            raise AsmError(f"line {n}: {e}\n    {raw}")
        listing.append(f"{len(out):02X}: {w:08X}  {raw}")
        out.append(w)
    return out, labels, "\n".join(listing)


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__.split("usage:")[1].strip())
        sys.exit(2)
    src_path, out_path, sym = sys.argv[1:4]
    bin_path = sys.argv[4] if len(sys.argv) == 5 else None
    try:
        words, labels, listing = assemble(open(src_path).read())
    except AsmError as e:
        print(f"{src_path}: {e}", file=sys.stderr)
        sys.exit(1)
    with open(out_path, "w") as f:
        f.write(f"/* generated by tools/dspasm.py from {src_path} - do not edit */\n")
        f.write(f"#define {sym.upper()}_LEN ({len(words)})\n")
        for k, v in sorted(labels.items(), key=lambda kv: kv[1]):
            f.write(f"#define {sym.upper()}_{k.upper()} ({v})\n")
        f.write("#ifndef DSP_PROG_SECTION\n#define DSP_PROG_SECTION\n#endif\n")
        f.write(f"static const unsigned int {sym}[{len(words)}] DSP_PROG_SECTION =\n{{\n")
        for i in range(0, len(words), 4):
            f.write("    " + ", ".join(f"0x{w:08X}" for w in words[i:i + 4]) + ",\n")
        f.write("};\n")
    if bin_path:
        with open(bin_path, "wb") as f:     # (the program as big-endian words, for the DSP's own DMA)
            for w in words:
                f.write(w.to_bytes(4, "big"))
    with open(out_path.rsplit(".", 1)[0] + ".lst", "w") as f:
        f.write(listing + "\n")
    print(f"{src_path}: {len(words)} words")


if __name__ == "__main__":
    main()
