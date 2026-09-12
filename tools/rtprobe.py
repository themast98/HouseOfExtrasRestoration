"""Read the render-target manager and decode the fields that decide whether the
final composite can put anything on screen.

Every pixel the player sees arrives through ONE blit: sub_CA5180(g_1A21AB8, 2)
at RVA 0x329175, immediately after the backbuffer is cleared to black. Inside
that blit:

    0xCA5296  call GetRT(g, 2)
    0xCA529B  test rax, rax
    0xCA529E  je   -> ebx = -1        (0xCA52A5 `or ebx, -1`)
    0xCA52A0  mov  ebx, [rax+8]       ; the RT texture id
    0xCA52B6  call sub_102EA50        ; id == -1 binds NOTHING

so a NULL RT-2 slot paints the cleared-black backbuffer while every upstream 2D
command is still built correctly - exactly the symptom at mission step 0x2C.

0x1A21AB8 holds a POINTER to the manager. Everything is read through it; nothing
is read or written at the global's own address. That distinction has already
cost this project one crash (GTextCtx) and one wrong diagnosis (g_layoutRes).

Usage:  python rtprobe.py <label>
"""

import re
import struct
import subprocess
import sys
import pathlib

HERE = pathlib.Path(__file__).parent
G_RT_MANAGER = 0x1A21AB8      # POINTER to the manager


def gcmd(*cmds):
    r = subprocess.run([sys.executable, str(HERE / "gcmd.py"), *cmds],
                       capture_output=True, text=True, timeout=120)
    return r.stdout


def parse(txt):
    buf = {}
    for ln in txt.splitlines():
        m = re.match(r'\s*([0-9A-F]{16})\s+((?:[0-9A-F]{2} )+)', ln)
        if m:
            a = int(m.group(1), 16)
            for i, c in enumerate(bytes.fromhex(m.group(2).replace(' ', ''))):
                buf[a + i] = c
    return buf


def grab(buf, addr, n):
    try:
        return bytes(buf[addr + i] for i in range(n))
    except KeyError:
        return None


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "snapshot"

    b = parse(gcmd("readrva %X 8" % G_RT_MANAGER))
    raw = next(iter(b.values())) if False else None
    addrs = sorted(b)
    if not addrs:
        print("ERR no reply from the game console")
        return 1
    A = struct.unpack('<Q', bytes(b[addrs[0] + i] for i in range(8)))[0]
    print("[%s] rt manager pointer = %016X" % (label, A))
    if not A:
        print("  manager pointer is NULL - nothing else can be read")
        return 0

    hdr = parse(gcmd("read %X 70" % A))
    h = grab(hdr, A, 0x70)
    if not h:
        print("  ERR could not read the manager header")
        return 1

    B = struct.unpack_from('<Q', h, 0x08)[0]
    mode = struct.unpack_from('<I', h, 0x30)[0]
    boundRT = struct.unpack_from('<Q', h, 0x18)[0]
    ext0 = struct.unpack_from('<f', h, 0x20)[0]
    ext1 = struct.unpack_from('<f', h, 0x24)[0]
    gate = struct.unpack_from('<I', h, 0x54)[0]

    print("  +0x08 slot array   = %016X" % B)
    print("  +0x18 bound RT     = %016X" % boundRT)
    print("  +0x20/+0x24 extents= %g / %g      <- the blit quad; 0 means degenerate" % (ext0, ext1))
    print("  +0x30 mode         = 0x%X          <- 0x27 means the present block ran this frame" % mode)
    print("  +0x54 composite gate = 0x%X" % gate)

    if not B:
        print("  slot array is NULL")
        return 0
    sl = parse(gcmd("read %X 20" % B))
    s = grab(sl, B, 0x20)
    if not s:
        print("  ERR could not read the slot array")
        return 1
    for i in range(4):
        v = struct.unpack_from('<Q', s, i * 8)[0]
        tag = "   <== eRT_INDEX_MAIN_2D (the only blit source)" if i == 2 else ""
        print("  slot[%d] = %016X%s" % (i, v, tag))
    rt2 = struct.unpack_from('<Q', s, 0x10)[0]
    if rt2:
        t = parse(gcmd("read %X 10" % rt2))
        tb = grab(t, rt2, 0x10)
        if tb:
            print("  rt2+0x08 texture id = 0x%X   <- -1 or 0 binds nothing" %
                  struct.unpack_from('<I', tb, 8)[0])
    else:
        print("  *** RT slot 2 is NULL -> the blit binds texture -1 and paints")
        print("      the cleared-black backbuffer. This is the cause.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
