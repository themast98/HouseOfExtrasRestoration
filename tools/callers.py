"""Find every call site of a function, following incremental-link thunks.

MSVC incremental linking routes most calls through `E9 rel32` thunks, so a naive
scan for `E8 rel32` pointing at the target finds almost nothing. This resolves
each call through up to 4 thunk hops before comparing, and reports the enclosing
function from .pdata.

Usage: python callers.py <dumped image> <target rva hex> [more rvas...]
"""

import bisect
import struct
import sys

TEXT_LO, TEXT_HI = 0x1000, 0x1204000


def load_funcs(img):
    """(start, end) ranges from .pdata, sorted by start."""
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    opt = struct.unpack_from("<H", img, pe + 20)[0]
    secs = pe + 24 + opt
    pd = None
    for i in range(nsec):
        o = secs + i * 40
        name = img[o:o + 8].rstrip(b"\0").decode(errors="replace")
        if name == ".pdata":
            pd = (struct.unpack_from("<I", img, o + 12)[0],
                  struct.unpack_from("<I", img, o + 8)[0])
    if not pd:
        return []
    va, sz = pd
    out = []
    for off in range(va, va + sz - 12, 12):
        s, e, _ = struct.unpack_from("<III", img, off)
        if s and e > s:
            out.append((s, e))
    out.sort()
    return out


def thunk(img, rva, hops=4):
    for _ in range(hops):
        if 0 <= rva < len(img) - 5 and img[rva] == 0xE9:
            rva = rva + 5 + struct.unpack_from("<i", img, rva + 1)[0]
        else:
            break
    return rva


def enclosing(funcs, starts, rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0:
        s, e = funcs[i]
        if s <= rva < e:
            return s, rva - s
        return s, None
    return None, None


def main():
    img = open(sys.argv[1], "rb").read()
    funcs = load_funcs(img)
    starts = [s for s, _ in funcs]

    for arg in sys.argv[2:]:
        target = int(arg, 16)
        print(f"=== callers of sub_{target:X} ===")
        hits = 0
        for i in range(TEXT_LO, min(TEXT_HI, len(img)) - 5):
            if img[i] != 0xE8:
                continue
            dest = i + 5 + struct.unpack_from("<i", img, i + 1)[0]
            if not (TEXT_LO <= dest < TEXT_HI):
                continue
            if thunk(img, dest) != target:
                continue
            fn, delta = enclosing(funcs, starts, i)
            where = (f"sub_{fn:X}+0x{delta:X}" if delta is not None
                     else f"0x{i:X} (leaf/no unwind, near sub_{fn:X})" if fn
                     else f"0x{i:X}")
            print(f"   {where}")
            hits += 1
        if not hits:
            print("   (none)")
        print()


if __name__ == "__main__":
    main()
