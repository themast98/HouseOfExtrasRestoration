"""Find PPC code that references a given address in the PS3 EBOOT.

SN Systems' PPC builds materialise addresses with an instruction PAIR rather
than storing pointers, so scanning for the little-endian value (or even the
big-endian value) finds nothing:

    lis  rX, ha16(addr)      # addis rX, r0, (addr + 0x8000) >> 16
    addi rX, rX, lo16(addr)  # signed low half, hence the +0x8000 bias
    ori  rX, rX, lo16(addr)  # unsigned variant

This scans .text for both forms, matches the reconstructed address, and reports
the hit. Also handles `lis` followed by a load using the low half as a
displacement (lwz/lbz/ld rY, lo(rX)).

Usage:
    python ps3xref.py <eboot.elf> <hex vaddr> [more vaddrs...]
    python ps3xref.py <eboot.elf> --str "some string"
"""

import struct
import sys

TEXT_OFF, TEXT_SIZE = 0x230, 0xE7BE28
VA_BIAS = 0x10000            # vaddr = fileoff + 0x10000


def va(off):
    return off + VA_BIAS


def off(vaddr):
    return vaddr - VA_BIAS


def find_string(data, s):
    needle = s.encode() + b"\x00"
    out, i = [], 0
    while True:
        i = data.find(needle, i)
        if i < 0:
            break
        out.append(i)
        i += 1
    return out


def scan(data, targets):
    """Return {target: [(vaddr_of_lis, reg, kind)]}."""
    hits = {t: [] for t in targets}
    # index the high halves we care about, for both bias conventions
    want = {}
    for t in targets:
        want.setdefault((t + 0x8000) >> 16, []).append(t)   # ha16 (addi)
        want.setdefault(t >> 16, []).append(t)              # hi16 (ori)

    end = TEXT_OFF + TEXT_SIZE
    for p in range(TEXT_OFF, end - 8, 4):
        w = struct.unpack_from(">I", data, p)[0]
        # addis rD, rA, SIMM  -> opcode 15; lis is addis with rA == 0
        if (w >> 26) != 15:
            continue
        rD = (w >> 21) & 31
        rA = (w >> 16) & 31
        if rA != 0:
            continue
        hi = w & 0xFFFF
        cands = want.get(hi)
        if not cands:
            continue
        # look ahead a few instructions for the matching low half in the same reg
        for q in range(p + 4, min(p + 4 * 12, end - 4), 4):
            w2 = struct.unpack_from(">I", data, q)[0]
            op2 = w2 >> 26
            d2 = (w2 >> 21) & 31
            a2 = (w2 >> 16) & 31
            lo = w2 & 0xFFFF
            if op2 == 14 and a2 == rD:            # addi rD, rX, lo  (signed)
                addr = (hi << 16) + struct.unpack(">h", struct.pack(">H", lo))[0]
                if addr in hits:
                    hits[addr].append((va(p), rD, "lis+addi"))
                break
            if op2 == 24 and a2 == rD:            # ori rD, rX, lo   (unsigned)
                addr = (hi << 16) | lo
                if addr in hits:
                    hits[addr].append((va(p), rD, "lis+ori"))
                break
            if op2 in (32, 34, 36, 38, 58) and a2 == rD:   # lwz/lbz/stw/stb/ld
                addr = (hi << 16) + struct.unpack(">h", struct.pack(">H", lo))[0]
                if addr in hits:
                    hits[addr].append((va(p), rD, f"lis+load(op{op2})"))
                break
            # the register was redefined before use: give up on this lis
            if d2 == rD and op2 in (14, 15, 24, 25):
                break
    return hits


def main():
    data = open(sys.argv[1], "rb").read()
    if sys.argv[2] == "--str":
        s = sys.argv[3]
        offs = find_string(data, s)
        if not offs:
            print(f"string {s!r} not found")
            return
        targets = [va(o) for o in offs]
        print(f"string {s!r} at " + ", ".join(f"0x{t:X}" for t in targets))
    else:
        targets = [int(a, 16) for a in sys.argv[2:]]

    hits = scan(data, targets)
    for t in targets:
        h = hits.get(t, [])
        print(f"\n0x{t:X}: {len(h)} reference(s)")
        for addr, reg, kind in h:
            print(f"   at 0x{addr:08X}  r{reg}  ({kind})")


if __name__ == "__main__":
    main()
