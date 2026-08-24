"""Generate robust AOB signatures for a list of function RVAs.

A naive "first N bytes" signature breaks whenever a rebuild moves anything,
because it bakes in rel32 branch targets and RIP-relative displacements. This
disassembles from the function start and replaces exactly those bytes with '?'
wildcards, then grows the pattern until it is unique in .text.

Usage:  python mkpat.py <dumped image> <base hex> NAME=RVA [NAME=RVA ...]
"""

import re
import sys

from capstone import *
from capstone.x86 import *

TEXT_LO, TEXT_HI = 0x1000, 0x1204000
MAX_LEN = 48


def masked_bytes(img, base, rva, upto):
    """Return [(byte, is_wildcard)] for instructions starting at rva."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    out = []
    off = rva
    while len(out) < upto:
        ins = next(iter(md.disasm(img[off:off + 16], base + off, 1)), None)
        if ins is None:
            break
        wild = [False] * ins.size
        # rel32 branches: opcode byte then 4 target bytes
        if ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j"):
            for op in ins.operands:
                if op.type == X86_OP_IMM and ins.size >= 5:
                    for k in range(ins.size - 4, ins.size):
                        wild[k] = True
        # RIP-relative memory operands: last 4 bytes of the instruction are disp32
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                for k in range(ins.size - 4, ins.size):
                    wild[k] = True
        for k in range(ins.size):
            out.append((ins.bytes[k], wild[k]))
        off += ins.size
    return out[:upto]


def to_regex(pairs):
    return b"".join(b"." if w else re.escape(bytes([b])) for b, w in pairs)


def to_str(pairs):
    return " ".join("?" if w else f"{b:02X}" for b, w in pairs)


def main():
    img = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 16)
    text = img[TEXT_LO:TEXT_HI]
    for arg in sys.argv[3:]:
        name, _, rvatxt = arg.partition("=")
        rva = int(rvatxt, 16)
        pairs = masked_bytes(img, base, rva, MAX_LEN)
        chosen = None
        for n in range(6, len(pairs) + 1):
            cand = pairs[:n]
            if all(w for _, w in cand[-1:]):      # never end on a wildcard
                continue
            hits = len(re.findall(to_regex(cand), text, re.S))
            if hits == 1:
                chosen = cand
                break
        if chosen:
            print(f'"{name}": {{ "pattern": "{to_str(chosen)}", "expect_rva": "0x{rva:X}" }},')
        else:
            print(f'// {name} @0x{rva:X}: NO UNIQUE PATTERN within {MAX_LEN} bytes')


if __name__ == "__main__":
    main()
