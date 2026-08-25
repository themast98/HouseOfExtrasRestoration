"""Offline validation of the address-resolution strategy.

These tests prove, without running the game, that:
  * the anchor byte-patterns are unique and land where we expect
  * reading call targets out of an anchor resolves every engine helper
  * walking RTTI reaches the ColosseumExtra vtable and its step handlers
  * the two patch sites really are `ret 0` stubs with int3 room to patch
  * the flag setter we call blind still has the shape we reverse-engineered

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


def _to_regex(pattern):
    return b"".join(
        b"." if t == "?" else re.escape(bytes([int(t, 16)]))
        for t in pattern.split()
    )


def _find_unique(img, pattern):
    """Match a pattern in .text, asserting it occurs exactly once."""
    hits = [m.start() + TEXT_LO
            for m in re.finditer(_to_regex(pattern), img[TEXT_LO:TEXT_HI], re.S)]
    assert len(hits) == 1, f"pattern has {len(hits)} hits, need exactly 1"
    return hits[0]


def _anchor_blocks(offsets):
    """The primary anchor plus every secondary anchor, as (name, block) pairs."""
    yield offsets["anchor"]["name"], offsets["anchor"]
    for name, blk in offsets.get("anchors", {}).items():
        yield name, blk


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
    assert _find_unique(img, a["pattern"]) == int(a["expect_rva"], 16)


def test_secondary_anchors_are_unique_and_correct(img, offsets):
    for name, blk in offsets.get("anchors", {}).items():
        got = _find_unique(img, blk["pattern"])
        assert got == int(blk["expect_rva"], 16), (
            f"{name}: matched 0x{got:X}, expected {blk['expect_rva']}"
        )


def test_standalone_patterns_are_unique_and_correct(img, offsets):
    """Every entry in the `patterns` block must match exactly once in .text and
    land on its expected RVA. Wildcards ('?') are allowed and are how we keep
    rel32 / RIP-relative bytes out of a signature."""
    pats = offsets.get("patterns", {})
    assert pats, "no standalone patterns declared"
    for name, p in pats.items():
        got = _find_unique(img, p["pattern"])
        assert got == int(p["expect_rva"], 16), (
            f"{name}: matched 0x{got:X}, expected {p['expect_rva']}"
        )


def test_every_anchor_resolves_its_calls_and_riprefs(img, offsets):
    """Covers the primary anchor and every secondary one in a single sweep, so a
    new anchor added to offsets.json is validated without editing this file."""
    checked = 0
    for aname, blk in _anchor_blocks(offsets):
        base = int(blk["expect_rva"], 16)
        for name, c in blk.get("calls", {}).items():
            got = _read_call(img, base + int(c["at"], 16))
            assert got == int(c["expect_rva"], 16), (
                f"{aname}.{name}: resolved 0x{got:X}, expected {c['expect_rva']}"
            )
            checked += 1
        for name, r in blk.get("riprefs", {}).items():
            got = _read_ripref(img, base + int(r["at"], 16), r["len"])
            assert got == int(r["expect_rva"], 16), (
                f"{aname}.{name}: resolved 0x{got:X}, expected {r['expect_rva']}"
            )
            checked += 1
    assert checked >= 10, f"only {checked} anchor targets checked - block lost?"


def test_flag_setter_has_the_shape_we_reverse_engineered(img, offsets):
    """We call SetFlagByAlias blind, so its contract must hold: read the flag
    manager's definition table at +0x990, then tail-jump to the raw setter that
    does the (group, bit) math. If a rebuild reshapes or inlines it, calling it
    would scribble on save flags - fail loudly here instead."""
    rva = int(offsets["anchors"]["DLC_FLAG_BRIDGE"]["calls"]["SET_FLAG_ALIAS"]["expect_rva"], 16)
    body = img[rva:rva + 0x50]
    table_field = int(offsets["constants"]["FLAGMGR_TABLE_FIELD"], 16)
    # mov rax, [rcx + <table_field>]
    want = b"\x48\x8b\x81" + struct.pack("<I", table_field)
    assert want in body, "setter no longer reads flagMgr+0x990"
    assert b"\xe9" in body, "setter no longer tail-jumps to the raw flag setter"


def test_dlc_mask_bits_are_the_missing_three(offsets):
    """The PC build ships ownership bits 0x00-0x16 and 0x1A but not 0x17-0x19,
    which is precisely why four of Bob's seven rows never appear. All three sit
    in dword 0 of the bitset, so a single OR sets them."""
    c = offsets["constants"]
    bits = [int(c[k], 16) for k in
            ("DLC_BIT_SURVIVAL_TAG_SP", "DLC_BIT_SPEED_KING", "DLC_BIT_FASTEST_KILLER")]
    assert bits == [0x17, 0x18, 0x19]
    assert all(b >> 5 == 0 for b in bits)


def test_unlock_aliases_are_consecutive_and_match_the_bridge(img, offsets):
    """The bridge hardcodes alias 0x648..0x64B against DLC bits 0x17..0x1A. Our
    three aliases must be the first three of that run - that correspondence is
    the whole reason we trust the alias numbers."""
    c = offsets["constants"]
    aliases = [int(c[k], 16) for k in
               ("FLAG_ALIAS_SURVIVAL_TAG_SP", "FLAG_ALIAS_SPEED_KING",
                "FLAG_ALIAS_FASTEST_KILLER")]
    assert aliases == [0x648, 0x649, 0x64A]

    # and the bridge really does pair each DLC bit with its alias, in order
    base = int(offsets["anchors"]["DLC_FLAG_BRIDGE"]["expect_rva"], 16)
    body = img[base:base + 0x9C]
    for dlc_bit, alias in zip((0x17, 0x18, 0x19, 0x1A), (0x648, 0x649, 0x64A, 0x64B)):
        # mov edx, <dlc bit>   ... later ...   mov edx, <alias>
        assert b"\xba" + struct.pack("<I", dlc_bit) in body, f"DLC bit 0x{dlc_bit:X} absent"
        assert b"\xba" + struct.pack("<I", alias) in body, f"alias 0x{alias:X} absent"


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
