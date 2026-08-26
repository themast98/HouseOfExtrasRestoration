"""Find every uniquely-gutted virtual method in the binary.

Sega shipped Yakuza 4 PC with some source files deleted; the affected virtual
methods survive as `C2 00 00` (ret 0) stubs. But `ret 0` alone proves nothing -
most are shared base-class defaults for virtuals a class does not override, used
by hundreds of vtables. The signature of DELETED code is a `ret 0` body reached
from exactly ONE vtable slot in the whole image.

Rather than walk RTTI per class (a full scan each time), this builds two indexes
in linear passes:
  1. every Complete Object Locator (signature 1, pSelf == its own offset)
  2. every qword pointing at a COL - the vtable begins 8 bytes later

Usage: python gutted.py <dumped image> [base hex]
"""

import collections
import struct
import sys

TEXT_LO, TEXT_HI = 0x1000, 0x1204000
MAX_SLOTS = 64


def main():
    img = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x7FF6F6D00000
    size = len(img)

    # ---- pass 1: complete object locators, and their type names ----
    col_name = {}
    for off in range(0, size - 24, 4):
        if struct.unpack_from("<I", img, off)[0] != 1:
            continue
        if struct.unpack_from("<I", img, off + 20)[0] != off:
            continue
        td = struct.unpack_from("<I", img, off + 12)[0]
        if not (0 < td < size - 16):
            continue
        end = img.find(b"\0", td + 16, td + 160)
        if end < 0:
            continue
        nm = img[td + 16:end]
        if nm[:4] != b".?AV":
            continue
        col_name[off] = nm.decode("ascii", "replace")
    print(f"indexed {len(col_name)} complete object locators")

    # ---- pass 2: vtables (a qword pointing at a COL; table starts after it) ----
    vtables = {}
    for j in range(0, size - 8, 8):
        q = struct.unpack_from("<Q", img, j)[0]
        c = q - base
        if c in col_name:
            vtables[j + 8] = col_name[c]
    print(f"found {len(vtables)} vtables")

    def thunk(rva):
        for _ in range(4):
            if TEXT_LO <= rva < TEXT_HI - 5 and img[rva] == 0xE9:
                rva = rva + 5 + struct.unpack_from("<i", img, rva + 1)[0]
            else:
                break
        return rva

    # ---- walk every vtable, counting how many slots reach each ret-0 body ----
    users = collections.Counter()
    entries = []
    for vt, name in vtables.items():
        for i in range(MAX_SLOTS):
            p = vt + i * 8
            if p + 8 > size:
                break
            q = struct.unpack_from("<Q", img, p)[0]
            rva = q - base
            if not (TEXT_LO <= rva < TEXT_HI):
                break
            f = thunk(rva)
            if img[f:f + 3] == b"\xc2\x00\x00":
                users[f] += 1
                entries.append((name, i, f))

    gutted = collections.defaultdict(list)
    for name, i, f in entries:
        if users[f] == 1:
            gutted[name].append((i, f))

    print(f"\n{len(gutted)} classes contain a UNIQUELY gutted virtual "
          f"(a ret-0 body no other vtable slot reaches):\n")
    for name in sorted(gutted):
        slots = sorted(gutted[name])
        print(f"  {name}")
        for i, f in slots:
            print(f"      slot {i:3}  sub_{f:X}")


if __name__ == "__main__":
    main()
