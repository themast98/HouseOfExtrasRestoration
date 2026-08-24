"""Offline validation of the address-resolution strategy.

These tests prove, without running the game, that:
  * the anchor byte-pattern is unique and lands where we expect
  * reading call targets out of the anchor resolves every engine helper
  * walking RTTI reaches the ColosseumExtra vtable and its step handlers
  * the two patch sites really are `ret 0` stubs with int3 room to patch

If these pass, the runtime resolver in src/resolve.cpp will find the same things.
"""

import json
import pathlib
import re
import struct

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
IMG_PATH = ROOT / "refs" / "Yakuza4_dumped.bin"

# Base the reference dump was taken at. Only used to interpret absolute
# pointers stored inside the image (vftable entries, COL self-refs).
DUMP_BASE = 0x7FF6F6D00000
TEXT_LO, TEXT_HI = 0x1000, 0x1204000


@pytest.fixture(scope="module")
def img():
    if not IMG_PATH.exists():
        pytest.skip("refs/Yakuza4_dumped.bin missing - see refs/README.md")
    return IMG_PATH.read_bytes()


@pytest.fixture(scope="module")
def offsets():
    return json.loads((ROOT / "offsets.json").read_text())


# --- helpers: the exact algorithms src/resolve.cpp must mirror ---------------

def _thunk(img, rva):
    """Follow incremental-link jmp thunks (E9 rel32), max 4 hops."""
    for _ in range(4):
        if 0 <= rva < len(img) and img[rva] == 0xE9:
            rva = rva + 5 + struct.unpack_from("<i", img, rva + 1)[0]
        else:
            break
    return rva


def _read_call(img, rva):
    """Resolve the target of a `call rel32` at rva."""
    assert img[rva] == 0xE8, f"no call at 0x{rva:X}, found {img[rva]:02X}"
    return _thunk(img, rva + 5 + struct.unpack_from("<i", img, rva + 1)[0])


def _read_ripref(img, rva, length):
    """Resolve a RIP-relative operand whose disp32 is the last 4 bytes."""
    return rva + length + struct.unpack_from("<i", img, rva + length - 4)[0]


def _find_vtable(img, type_name):
    """RTTI type descriptor -> complete object locator -> vftable RVA."""
    needle = type_name.encode() + b"\x00"
    assert img.count(needle) == 1, f"{type_name} is not unique in the image"
    td = img.find(needle) - 16
    for off in range(0, len(img) - 24, 4):
        if struct.unpack_from("<I", img, off)[0] != 1:          # signature
            continue
        if struct.unpack_from("<I", img, off + 12)[0] != td:    # pTypeDescriptor
            continue
        if struct.unpack_from("<I", img, off + 20)[0] != off:   # pSelf
            continue
        ptr = img.find(struct.pack("<Q", DUMP_BASE + off))
        if ptr != -1:
            return ptr + 8
    raise AssertionError(f"no vftable found for {type_name}")


def _slot(img, vt, index):
    q = struct.unpack_from("<Q", img, vt + index * 8)[0] - DUMP_BASE
    return _thunk(img, q)


# --- tests ------------------------------------------------------------------

def test_anchor_pattern_is_unique_and_correct(img, offsets):
    a = offsets["anchor"]
    rx = b"".join(
        b"." if t == "?" else re.escape(bytes([int(t, 16)]))
        for t in a["pattern"].split()
    )
    hits = [m.start() for m in re.finditer(rx, img[TEXT_LO:TEXT_HI], re.S)]
    assert len(hits) == 1, f"anchor pattern has {len(hits)} hits, need exactly 1"
    assert hits[0] + TEXT_LO == int(a["expect_rva"], 16)


def test_standalone_patterns_are_unique_and_correct(img, offsets):
    """Every entry in the `patterns` block must match exactly once in .text and
    land on its expected RVA. Wildcards ('?') are allowed and are how we keep
    rel32 / RIP-relative bytes out of a signature."""
    pats = offsets.get("patterns", {})
    assert pats, "no standalone patterns declared"
    text = img[TEXT_LO:TEXT_HI]
    for name, p in pats.items():
        rx = b"".join(
            b"." if t == "?" else re.escape(bytes([int(t, 16)]))
            for t in p["pattern"].split()
        )
        hits = [m.start() + TEXT_LO for m in re.finditer(rx, text, re.S)]
        assert len(hits) == 1, f"{name}: {len(hits)} hits, need exactly 1"
        assert hits[0] == int(p["expect_rva"], 16), (
            f"{name}: matched 0x{hits[0]:X}, expected {p['expect_rva']}"
        )


def test_anchor_calls_resolve(img, offsets):
    base = int(offsets["anchor"]["expect_rva"], 16)
    for name, c in offsets["anchor"]["calls"].items():
        got = _read_call(img, base + int(c["at"], 16))
        assert got == int(c["expect_rva"], 16), (
            f"{name}: resolved 0x{got:X}, expected {c['expect_rva']}"
        )


def test_anchor_riprefs_resolve(img, offsets):
    base = int(offsets["anchor"]["expect_rva"], 16)
    for name, r in offsets["anchor"]["riprefs"].items():
        got = _read_ripref(img, base + int(r["at"], 16), r["len"])
        assert got == int(r["expect_rva"], 16), (
            f"{name}: resolved 0x{got:X}, expected {r['expect_rva']}"
        )


def test_rtti_slots_resolve(img, offsets):
    for cls, info in offsets["rtti"].items():
        vt = _find_vtable(img, info["type_name"])
        for name, s in info["slots"].items():
            got = _slot(img, vt, s["slot"])
            assert got == int(s["expect_rva"], 16), (
                f"{cls}.{name} slot {s['slot']}: resolved 0x{got:X}, "
                f"expected {s['expect_rva']}"
            )


def test_patch_sites_are_stubs_with_room(img, offsets):
    for name, s in offsets["rtti"]["COLOSSEUM_EXTRA"]["slots"].items():
        rva = int(s["expect_rva"], 16)
        assert img[rva:rva + 3] == b"\xc2\x00\x00", f"{name} is not a `ret 0` stub"
        tail = img[rva + 3: rva + s["space"]]
        assert set(tail) <= {0xCC}, f"{name}: tail is not int3 padding"
        assert s["space"] >= 12, f"{name}: needs >=12 bytes for an absolute jump"
