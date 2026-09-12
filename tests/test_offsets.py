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
        # A pattern may deliberately anchor somewhere other than the function
        # entry - SET_PANE_HIDDEN matches its distinguishing TAIL because five
        # functions share its prologue byte for byte. `offset` says how far the
        # match sits from the entry.
        got += int(p.get("offset", "0"), 16)
        assert got == int(p["expect_rva"], 16), (
            f"{name}: resolved 0x{got:X}, expected {p['expect_rva']}"
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


def test_texpar_table_is_an_absolute_displacement(img, offsets):
    """The layout texture-archive table is reached as [reg + idx*8 + disp32]
    with a zeroed base, not RIP-relative - so the displacement IS the RVA and
    must not have the instruction length added. Getting that wrong silently
    reads the wrong table, so pin both the encoding and the address."""
    c = offsets["constants"]
    base = int(offsets["patterns"]["TEXPAR_LOAD"]["expect_rva"], 16)
    at = base + int(c["TEXPAR_TABLE_DISP_AT"], 16)
    # 49 8b 8c f6 <disp32>  =  mov rcx, [r14 + rsi*8 + disp32]
    assert img[at:at + 4] == b"\x49\x8b\x8c\xf6", (
        f"unexpected encoding at 0x{at:X}: {img[at:at + 4].hex(' ')}"
    )
    disp = struct.unpack_from("<I", img, at + 4)[0]
    assert disp == int(c["G_LAYOUT_TEXPAR_EXPECT"], 16), f"got 0x{disp:X}"


def test_layout_slot_count_fits_between_the_tables(offsets):
    """Three parallel per-slot tables sit back to back:
        0x19809E0 dword/slot   0x1980A80 qword/slot (textures)   0x1980D18 (resources)
    The dword table ends exactly where the texture table starts, which is what
    pins the slot count. The runtime bound must not exceed what any gap allows,
    or the diagnostic reads past a table into unrelated globals."""
    c = offsets["constants"]
    state = int(c["G_LAYOUT_STATE_EXPECT"], 16)
    texpar = int(c["G_LAYOUT_TEXPAR_EXPECT"], 16)
    res = int(offsets["anchors"]["NETRANK_BIND"]["riprefs"]["G_LAYOUT_RES"]["expect_rva"], 16)
    n = int(c["LAYOUT_SLOT_COUNT"])

    assert state + n * 4 <= texpar, "slot count overruns the dword table"
    assert texpar + n * 8 <= res, "slot count overruns the texture table"
    # the dword table ending flush against the texture table is the evidence
    assert state + n * 4 == texpar, "the dword table no longer ends at the texture table"


def test_layout_load_never_touches_textures(img, offsets):
    """The premise of the texture diagnostic: LoadLayout builds only '<name>.csb'
    and never calls the texture-archive loader. If a rebuild merged the two,
    the diagnostic's conclusion would be wrong and this should fail."""
    lo = int(offsets["patterns"]["LAYOUT_LOAD"]["expect_rva"], 16)
    tex = int(offsets["patterns"]["TEXPAR_LOAD"]["expect_rva"], 16)
    body = img[lo:lo + 0x400]
    for i in range(len(body) - 5):
        if body[i] != 0xE8:
            continue
        dest = _thunk(img, lo + i + 5 + struct.unpack_from("<i", body, i + 1)[0])
        assert dest != tex, f"LoadLayout calls the texture loader at +0x{i:X}"


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


def _all_mission_macro_classes(img):
    """Decorated RTTI names, which is what _find_vtable expects."""
    return sorted({m.group(0).decode()
                   for m in re.finditer(rb"\.\?AVCMissionMacro[A-Za-z0-9_]*@@", img)})


def test_only_colosseum_extra_has_gutted_step_handlers(img, offsets):
    """The scope claim the whole mod rests on.

    A macro's vtable has 96 slots; the step dispatcher has a 64-entry jump
    table, so slots 18..81 are the step handlers (slot 62 == step 0x2C pins the
    +18 offset). Most `ret 0` entries are shared base-class defaults - steps a
    given macro simply does not use - so `ret 0` alone proves nothing. The
    signature of code Sega deleted is a `ret 0` handler at an address used by
    exactly ONE class, sitting beside its still-real siblings.

    Sweeping every CMissionMacro class must therefore find exactly two: the
    pair we patch. If a future build guts another one, some other mode can hang
    the same way House of Extras did, and this fails loudly."""
    STEP_LO, STEP_HI = 18, 81
    classes = _all_mission_macro_classes(img)
    assert len(classes) > 50, f"only found {len(classes)} macro classes - RTTI scan broke"

    per_class, users, skipped = {}, {}, []
    for name in classes:
        try:
            vt = _find_vtable(img, name)
        except AssertionError as exc:
            # Never silently: a swallowed lookup would empty the sweep and make
            # this test claim "nothing else is gutted" without having looked.
            skipped.append(f"{name}: {exc}")
            continue
        stubs = {}
        for i in range(STEP_LO, STEP_HI + 1):
            rva = struct.unpack_from("<Q", img, vt + i * 8)[0] - DUMP_BASE
            if not (TEXT_LO <= rva < TEXT_HI):
                break
            f = _thunk(img, rva)
            if img[f:f + 3] == b"\xc2\x00\x00":
                stubs[i] = f
        per_class[name] = stubs
        for a in set(stubs.values()):
            users[a] = users.get(a, 0) + 1

    gutted = {
        name: {i: a for i, a in stubs.items() if users[a] == 1}
        for name, stubs in per_class.items()
    }
    gutted = {k: v for k, v in gutted.items() if v}

    assert len(per_class) >= len(classes) - 2, (
        f"resolved only {len(per_class)}/{len(classes)} vtables, so the sweep is "
        f"not conclusive. Skipped: {skipped[:5]}"
    )

    patched = offsets["rtti"]["COLOSSEUM_EXTRA"]
    expected_name = patched["type_name"]          # decorated, as _find_vtable wants
    # Patch sites only. MODE_TEARDOWN is a call target and is intact by design,
    # so including it here would assert that a live function is a `ret 0` stub.
    expected_slots = {int(s["slot"]) for s in patched["slots"].values()
                      if "space" in s}

    assert set(gutted) == {expected_name}, (
        f"gutted step handlers outside the class we patch: "
        f"{ {k: [hex(i) for i in v] for k, v in gutted.items()} }"
    )
    assert set(gutted[expected_name]) == expected_slots, (
        f"gutted slots {sorted(gutted[expected_name])} != patched {sorted(expected_slots)}"
    )


def _screen_class(img, offsets, screen_id):
    """Resolve a screen id the way OpenScreen does, then name the class its
    factory constructs: id -> range index -> factory -> ctor -> vftable -> RTTI."""
    c = offsets["constants"]
    lo_t = int(c["SCREEN_ID_RANGE_LO"], 16)
    hi_t = int(c["SCREEN_ID_RANGE_HI"], 16)
    tab = int(c["SCREEN_FACTORY_TABLE"], 16)

    idx = None
    for i in range(int(c["SCREEN_ID_RANGE_COUNT"])):
        lo = struct.unpack_from("<i", img, lo_t + i * 4)[0]
        hi = struct.unpack_from("<i", img, hi_t + i * 4)[0]
        if lo <= screen_id <= hi:
            idx = i
            break
    assert idx is not None, f"screen id {screen_id} matches no range"
    factory = _thunk(img, struct.unpack_from("<Q", img, tab + idx * 8)[0] - DUMP_BASE)

    # Walk the factory and the ctors it calls, looking for a stored vftable.
    seen, stack = set(), [(factory, 0)]
    while stack:
        fn, depth = stack.pop(0)
        if fn in seen or depth > 2:
            continue
        seen.add(fn)
        for off in range(fn, min(fn + 0x180, TEXT_HI - 8)):
            # lea rax, [rip+disp] : 48 8D 05 disp32
            if img[off:off + 3] == b"\x48\x8d\x05":
                tgt = off + 7 + struct.unpack_from("<i", img, off + 3)[0]
                try:
                    col = struct.unpack_from("<Q", img, tgt - 8)[0] - DUMP_BASE
                    td = struct.unpack_from("<I", img, col + 12)[0]
                    end = img.find(b"\0", td + 16, td + 200)
                    nm = img[td + 16:end].decode("ascii", "replace")
                    if nm.startswith(".?AV"):
                        return idx, factory, nm
                except Exception:
                    pass
            if img[off] == 0xE8:                        # call rel32
                stack.append((_thunk(img, off + 5 + struct.unpack_from("<i", img, off + 1)[0]),
                              depth + 1))
    return idx, factory, None


def test_screen_ids_select_the_classes_we_think(img, offsets):
    """The crux of the recap fix, and the thing that was wrong for a long time.

    OpenScreen's id is not a "screen number" - it selects a CLASS through a
    factory table, and 220 is a CAPTION BANNER, not a recap. Opening it gave a
    perfectly healthy object that simply had nothing to show. If a rebuild ever
    renumbers these, the mod would silently drive the wrong object again."""
    c = offsets["constants"]
    expected = {
        int(c["SCREEN_ID_CAPTION"]):          "CActionSurvivalCaption",
        int(c["SCREEN_ID_BATTLE_RESULT"]):    "CActionSurvivalBattleResult",
        int(c["SCREEN_ID_ONIGOKKO_RESULT"]):  "CActionSurvivalOnigokkoResult",
        int(c["SCREEN_ID_TOUGIJYO_RESULT"]):  "CActionTougijyoAllStarResult",
    }
    for sid, want in expected.items():
        idx, factory, got = _screen_class(img, offsets, sid)
        assert got == f".?AV{want}@@", (
            f"screen {sid} (idx {idx}, factory sub_{factory:X}) builds {got}, expected {want}"
        )

    # and the mod must default to the recap, never the caption
    assert int(c["SCREEN_ID_BATTLE_RESULT"]) != int(c["SCREEN_ID_CAPTION"])


def test_patch_sites_are_stubs_with_room(img, offsets):
    """Only slots declaring `space` are patch sites we overwrite.

    COLOSSEUM_EXTRA also declares MODE_TEARDOWN, which we CALL rather than
    patch, and which must emphatically not be a stub - see its own test below."""
    sites = {n: s for n, s in offsets["rtti"]["COLOSSEUM_EXTRA"]["slots"].items()
             if "space" in s}
    assert sites, "no patch sites declared"
    for name, s in sites.items():
        rva = int(s["expect_rva"], 16)
        assert img[rva:rva + 3] == b"\xc2\x00\x00", f"{name} is not a `ret 0` stub"
        tail = img[rva + 3: rva + s["space"]]
        assert set(tail) <= {0xCC}, f"{name}: tail is not int3 padding"
        assert s["space"] >= 12, f"{name}: needs >=12 bytes for an absolute jump"


def test_mode_teardown_slot_is_intact_and_has_both_branches(img, offsets):
    """The fix calls this function; if it were gutted the fix would be a no-op.

    PS3 CMissionMacroColosseumExtra::Step2D ends by invoking vtable slot 10 and
    then SetNextStep(this, 0). On PC that slot is +0xB58EE0, and unlike slots
    62/63 it was NEVER deleted - only its caller was. So it must NOT be a stub,
    and it must still branch on its int argument: kind == 0 takes the abort path
    (which routes itself to 0x2E), kind != 0 takes the completion path that
    releases the mode and writes the mode's finished flag through the flag
    manager. Those two reads - flagMgr->[0x990] and the +0xD08/+0xD0A flag id
    pair - are exactly what the mod's guards protect, so pin them here."""
    s = offsets["rtti"]["COLOSSEUM_EXTRA"]["slots"]["MODE_TEARDOWN"]
    assert "space" not in s, "MODE_TEARDOWN is a call target, not a patch site"

    rva = int(s["expect_rva"], 16)
    assert img[rva:rva + 3] != b"\xc2\x00\x00", (
        "the teardown is a `ret 0` stub - the whole fix would be a no-op"
    )

    body = img[rva:rva + 0xB0]
    assert b"\x85\xd2" in body[:0x20], "no `test edx,edx` - the kind branch is gone"
    assert b"\x90\x09\x00\x00" in body, (
        f"no flagMgr->[{offsets['constants']['FLAGMGR_TABLE_FIELD']}] load in the "
        f"completion path"
    )
    assert b"\x08\x0d\x00\x00" in body and b"\x0a\x0d\x00\x00" in body, (
        "the +0xD08/+0xD0A flag-id reads are gone"
    )


def test_texture_residency_gate_has_the_shape_we_diagnosed(img, offsets):
    """The whole reason the panel was invisible: the renderer picks a texture id
    from pane+0x4C (falling back to RS+0x1C), stores it into the draw command,
    asks IsTexResident about it, and on a false answer branches away - after
    sub_4889C0 has already transformed the quad's four corners. That is why
    enlarging the sprites to full screen still lit no pixels.

    If a rebuild moves or reshapes this, the fix in netrank::Draw() is stamping
    a value nothing reads any more, so fail loudly rather than silently.
    """
    gate = 0x486D79
    want = bytes.fromhex("8b434c85c07503"     # mov eax,[rbx+0x4C]; test eax,eax; jne +3
                         "8b471c"             # mov eax,[rdi+0x1C]      (fallback)
                         "8bc8"               # mov ecx,eax
                         "894640")            # mov [rsi+0x40],eax      (drawcmd texture id)
    assert img[gate:gate + len(want)] == want, (
        f"the texture gate at 0x{gate:X} is not the sequence we diagnosed: "
        f"{img[gate:gate + len(want)].hex()}"
    )
    # ...then the residency call, then a conditional branch straight past the emit.
    call_at = gate + len(want)
    assert img[call_at] == 0xE8, "expected the IsTexResident call right after the store"
    dest = _thunk(img, call_at + 5 + struct.unpack_from("<i", img, call_at + 1)[0])
    assert dest == int(offsets["patterns"]["IS_TEX_RESIDENT"]["expect_rva"], 16), (
        f"the gate calls 0x{dest:X}, not IS_TEX_RESIDENT"
    )
    assert img[call_at + 5:call_at + 7] == bytes.fromhex("85c0"), "expected test eax,eax"
    assert img[call_at + 7] == 0x74, "expected a je skipping the emit on a false result"


def test_set_tex_handle_is_a_single_store_to_elem_0x4e(img, offsets):
    """netrank::Draw() writes elem+0x4E directly instead of calling this, on the
    grounds that they are the same instruction and a direct store cannot be
    mis-shaped. That reasoning only holds while this really is a bare
    `mov [rcx+0x4E], dx ; ret`."""
    rva = int(offsets["anchors"]["PAGE_CTOR"]["calls"]["SET_TEX_HANDLE"]["expect_rva"], 16)
    assert img[rva:rva + 5] == bytes.fromhex("6689514ec3"), (
        f"SET_TEX_HANDLE is not `mov [rcx+0x4E],dx ; ret`: {img[rva:rva + 8].hex()}"
    )


def test_shared_tex_getters_are_adjacent_stubs(img, offsets):
    """resolve.cpp derives SharedTexA as SharedTexB + 0x20 because neither stub
    has a prefix worth pattern-matching. It guards that with an opcode check at
    runtime; this pins the layout assumption itself."""
    b = int(offsets["anchors"]["PAGE_CTOR"]["calls"]["SHARED_TEX_B"]["expect_rva"], 16)
    for rva in (b, b + 0x20):
        assert img[rva] == 0x8B and img[rva + 1] == 0x05, (
            f"0x{rva:X} is not `mov eax,[rip+d32]`: {img[rva:rva + 8].hex()}"
        )
        assert img[rva + 6] == 0xC3, f"0x{rva:X} does not return immediately"


def test_steam_achievement_call_sites_have_the_shape_steamfix_requires(img, offsets):
    """steamfix.cpp rewrites two 5-byte `E8 rel32` calls and refuses to touch
    anything unless each is preceded 15 bytes earlier by `mov edx, <field size>`.
    That guard is only meaningful while the sites really do look like this.

    This works around a crash in the STOCK game - reproduced 6/6 with every mod
    removed - so the sites belong to Sega's code, not ours, and could in
    principle move under a game patch. If this test ever fails, the runtime
    check in InstallSteamAchievementFix will also refuse, which is the safe
    outcome: no patch, and the vanilla crash comes back.
    """
    c = offsets["constants"]
    thunk = int(c["STEAM_SNPRINTF_THUNK"], 16)
    # The `lea rcx` that sets the destination sits at a different distance in each
    # site (disp8 for m_rgchName at +0x44, disp32 for m_rgchDescription at +0xC4),
    # so it is pinned by its exact bytes rather than a shared offset.
    for key, size, lea in (
        ("STEAM_ACHV_CALL_NAME", 0x80, "488d4e44"),          # lea rcx,[rsi+0x44]
        ("STEAM_ACHV_CALL_DESC", 0x100, "488d8ec4000000"),   # lea rcx,[rsi+0xC4]
    ):
        rva = int(c[key], 16)
        assert img[rva] == 0xE8, f"{key} is not a rel32 call: {img[rva:rva + 5].hex()}"
        target = rva + 5 + struct.unpack_from("<i", img, rva + 1)[0]
        assert target == thunk, f"{key} calls 0x{target:X}, not the snprintf thunk"
        # `mov edx, imm32` - the destination buffer's size
        assert img[rva - 15] == 0xBA, f"{key} is not preceded by `mov edx, imm32`"
        assert struct.unpack_from("<I", img, rva - 14)[0] == size, (
            f"{key} does not pass size 0x{size:X}"
        )
        # `mov r9, rax` - the Steam display-attribute pointer becomes the vararg.
        # This is the argument our replacement copies, so it is the load-bearing one.
        assert img[rva - 10:rva - 7] == bytes.fromhex("4c8bc8"), (
            f"{key} is not preceded by `mov r9, rax`: {img[rva - 10:rva - 7].hex()}"
        )
        # `lea rcx, [rsi + <field>]` - the destination inside Achievement_t
        want = bytes.fromhex(lea)
        start = rva - 15 - len(want)
        assert img[start:rva - 15] == want, (
            f"{key}: expected {lea} before the size, got {img[start:rva - 15].hex()}"
        )

    # The thunk both sites reach is a plain jmp, and the faulting strlen really is
    # the unaligned SSE4.2 loop we diagnosed.
    assert img[thunk] == 0xE9, "the snprintf thunk is not a jmp"
    fault = int(c["STEAM_STRLEN_FAULT"], 16)
    assert img[fault:fault + 5] == bytes.fromhex("c5fa6f4010"), (
        f"0x{fault:X} is not `vmovdqu xmm0,[rax+0x10]`: {img[fault:fault + 8].hex()}"
    )
    assert img[fault + 9:fault + 15] == bytes.fromhex("c4e37963c814"), (
        "the vpcmpistri that makes this an unaligned strlen is not where we think"
    )


def test_no_mod_patch_lands_anywhere_near_the_achievement_array(img, offsets):
    """The crash we work around is in Sega's code. Pin the fact that nothing this
    mod writes overlaps the achievement array or the crash path, so a future
    patch site cannot quietly grow into them."""
    c = offsets["constants"]
    array = int(c["STEAM_ACHV_ARRAY"], 16)
    danger = [
        ("Achievement_t array", array, 47 * 0x1CC),
        ("printf core", 0xEFA730, 0xEFBC5D - 0xEFA730),
    ]
    writes = [
        (int(c["CHASE_DRAIN_ABILITY_TEST"], 16), 4),
        (int(c["CHASE_DRAIN_THRESHOLD"], 16), 1),
        (int(c["SURVIVE_RANK_BRANCH"], 16), 1),
    ]
    for wa, wl in writes:
        for name, da, dl in danger:
            assert not (wa < da + dl and da < wa + wl), (
                f"a mod write at 0x{wa:X} overlaps {name}"
            )


def test_chase_drain_gate_is_the_shape_the_patch_assumes(img, offsets):
    """FixChaseDrainRate NOPs an ability test so the engine's own frame-skip gate
    becomes unconditional, then rewrites its threshold to "N frames out of 10".

    This replaced an earlier fix that scaled four constants in sub_604760 - which
    turned out to be CFighter::ApplyPendingDamage, an event-driven per-hit damage
    routine. That patch applied cleanly, logged success every second, changed the
    chase not at all, and halved real combat damage. Hence this test: pin the
    shape of the site that is actually the drain.
    """
    c = offsets["constants"]
    fn = int(c["CHASE_DRAIN_FUNC"], 16)
    test_rva = int(c["CHASE_DRAIN_ABILITY_TEST"], 16)
    thr_rva = int(c["CHASE_DRAIN_THRESHOLD"], 16)

    # `mov edi, 1` at the top: the default is "drain every frame".
    assert img[fn + 0x0A:fn + 0x0F] == bytes.fromhex("bf01000000"), (
        "sub_5A4810 does not start by defaulting the gate to 1"
    )
    # HasAbility(0x4C) via `lea edx,[rdi+0x4b]`, then the test/je we NOP.
    assert img[fn + 0x12:fn + 0x15] == bytes.fromhex("8d574b"), (
        "expected `lea edx,[rdi+0x4b]` (ability 0x4C) before the test"
    )
    assert img[test_rva:test_rva + 4] == bytes.fromhex("85c0742a"), (
        f"drain ability test is not `test eax,eax / je`: {img[test_rva:test_rva + 4].hex()}"
    )
    # The threshold is the imm8 of `cmp r8d, 7`.
    assert img[thr_rva - 3:thr_rva + 1] == bytes.fromhex("4183f807"), (
        f"drain threshold is not `cmp r8d, 7`: {img[thr_rva - 3:thr_rva + 1].hex()}"
    )
    # The je we NOP must land past the %10 block, and nothing may branch INTO the
    # four bytes we overwrite.
    # je is at test_rva+2 and is itself 2 bytes, so it lands at test_rva+4+rel8.
    assert test_rva + 4 + 0x2A == fn + 0x48, "the je does not skip to the 0x4E path"

    # The gate that actually skips the subtraction, and the record-flag bypass
    # that must stay reachable (rec[0x44] & 0x20 is 0 in every shipped record).
    assert img[fn + 0x8C:fn + 0x90] == bytes.fromhex("f6404420"), (
        "expected `test byte [rax+0x44], 0x20` (the record bypass)"
    )
    assert img[fn + 0x92:fn + 0x94] == bytes.fromhex("85ff"), (
        "expected `test edi,edi` as the frame gate"
    )


def test_survive_step_2c_is_straight_line_with_one_openscreen_call(img, offsets):
    """The Endless result-screen suppression lets the engine's step 0x2C run
    with its single OpenScreen call NOPped for that one synchronous call. That
    only holds while 0x2C is the straight-line body we decoded: `mov edx, 223`
    twelve bytes before an E8 that reaches OpenScreen through the link thunk,
    and a class-unique vtable slot 62 so the hook cannot touch another macro."""
    c = offsets["constants"]
    fn = int(c["SURVIVE_STEP2C_FUNC"], 16)
    mov = int(c["SURVIVE_SCREEN_ID_MOV"], 16)
    call = int(c["SURVIVE_OPENSCREEN_CALL"], 16)
    open_screen = int(offsets["anchor"]["calls"]["OPEN_SCREEN"]["expect_rva"], 16)

    assert img[mov:mov + 5] == bytes.fromhex("badf000000"), (
        f"expected `mov edx, 223` at 0x{mov:X}: {img[mov:mov + 5].hex()}"
    )
    assert call == mov + 12, "the call is not 12 bytes after the screen-id mov"
    assert _read_call(img, call) == open_screen, "step 0x2C's call does not reach OpenScreen"
    # Straight-line to SetNextStep(0x2D): the very next instruction after the call.
    assert img[call + 5:call + 10] == bytes.fromhex("ba2d000000"), (
        "expected `mov edx, 0x2D` right after the OpenScreen call"
    )
    # The refcount call with edx=1 that makes routing AROUND 0x2C unsafe.
    assert img[fn + 0x09:fn + 0x0E] == bytes.fromhex("ba01000000"), (
        "expected `mov edx, 1` (refcount increment) at the top of step 0x2C"
    )

    vt = _find_vtable(img, ".?AVCMissionMacroAdventureSurvive@@")
    slot = int(c["SURVIVE_STEP2C_SLOT"], 16) if c["SURVIVE_STEP2C_SLOT"].startswith("0x") \
        else int(c["SURVIVE_STEP2C_SLOT"])
    assert _slot(img, vt, slot) == fn, "slot 62 of AdventureSurvive is not step 0x2C"
    raw = struct.unpack_from("<Q", img, vt + slot * 8)[0]
    assert img.count(struct.pack("<Q", raw)) == 1, (
        "step 0x2C's thunk appears in more than one vtable - not class-unique"
    )


def test_logic_counter_pause_gate_bytes(img, offsets):
    """ReportChaseDrainClock discards any window in which the pause bits are set,
    because the logic frame counter at g_MainMgr+0x1C is incremented only when
    they are clear. Pin the gate that establishes both facts."""
    c = offsets["constants"]
    bits = int(c["MAINMGR_PAUSE_BITS"], 16)
    mask = int(c["MAINMGR_PAUSE_MASK"], 16)
    frame = int(c["MAINMGR_LOGIC_FRAME"], 16)
    at = 0x3293F6
    # test dword ptr [rsi + bits], mask
    want = b"\xf7\x86" + struct.pack("<I", bits) + struct.pack("<I", mask)
    assert img[at:at + 10] == want, f"pause gate is not `test [rsi+0x{bits:X}], 0x{mask:X}`"
    assert img[at + 10:at + 12] == bytes.fromhex("7503"), "expected `jne +3` after the test"
    # inc dword ptr [rsi + 0x1C]
    assert img[at + 12:at + 15] == b"\xff\x46" + bytes([frame]), (
        "the guarded increment is not on the logic frame counter"
    )


def test_escape_step_0xe_branches_on_the_intro_done_field(img, offsets):
    """EscapeReplayIntro zeroes macro+0x130 once per ranked run because Escape's
    step 0xE reads it as "already ran": ==0 restarts from step 0 (the title card
    and countdown), !=0 jumps to step 0x14 (straight into the chase). Pin both
    branches so the write keeps meaning what the comment says."""
    field = int(offsets["constants"]["MACRO_INTRO_DONE"], 16)
    vt = _find_vtable(img, ".?AVCMissionMacroAdventureEscape@@")
    fn = _slot(img, vt, 0x0E + 18)
    body = img[fn:fn + 0xC0]
    # cmp dword ptr [rbx + 0x130], 0 ; je <zero path>
    cmp = b"\x83\xbb" + struct.pack("<I", field) + b"\x00"
    i = body.find(cmp)
    assert i != -1, "step 0xE no longer compares macro+0x130 against 0"
    # cmp (7) ; mov rcx,rbx (3) ; mov rax,[rbx] (3) ; je - the vtable loads sit
    # between the compare and the branch.
    assert body[i + 7:i + 13] == bytes.fromhex("488bcb488b03"), "unexpected code between cmp and je"
    assert body[i + 13] == 0x74, "expected a je after the +0x130 compare"
    # non-zero path: mov edx, 3 ; call [rax+0x38]  (dispatcher: 3 -> SetStep(0x14))
    assert b"\xba\x03\x00\x00\x00\xff\x50\x38" in body, "non-zero path is not vf[0x38](3)"
    # zero path: call [rax+0x2f0] ; xor edx,edx ; ... ; jmp SetStep  (-> step 0)
    assert b"\xff\x90\xf0\x02\x00\x00\x33\xd2" in body, "zero path is not vf[0x2F0]; edx=0"
    # and the dispatcher really maps 3 -> 0x14
    disp = _slot(img, vt, 7)
    assert img[disp:disp + 3] == bytes.fromhex("83ea01"), "vf[0x38] is not the type dispatcher"
    assert b"\xba\x14\x00\x00\x00" in img[disp:disp + 0x40], "dispatcher does not map a type to step 0x14"


def test_chase_intro_latch_shape(img, offsets):
    """+0x299B70 must still be the one-shot intro latch on CActionChase+0x782:
    movzx eax,[rcx+0x782]; test al,0x20; jne; or al,0x20; mov [rcx+0x782],al."""
    c = offsets["constants"]
    fn = int(c["CHASE_INTRO_LATCH"], 16)
    fld = int(c["CHASE_FLAGS_782"], 16)
    bit = int(c["CHASE_INTRO_BIT"], 16)
    assert fld == 0x782 and bit == 0x20
    b = img[fn:fn + 19]
    disp = fld.to_bytes(4, "little")
    assert b[0:3] == b"\x0f\xb6\x81" and b[3:7] == disp          # movzx eax, byte [rcx+0x782]
    assert b[7:9] == bytes([0xA8, bit])                          # test al, 0x20
    assert b[9] == 0x75                                          # jne (early return)
    assert b[11:13] == bytes([0x0C, bit])                        # or al, 0x20
    assert b[13:16] == b"\x88\x81" + disp[:1] and b[15:19] == disp  # mov [rcx+0x782], al


def test_escape_step4_sets_the_start_caption_behind_the_gate_we_trace(img, offsets):
    """DiagCaptionTrace hooks AdventureEscape slot 22 (step 4) because it is where
    the Speed King title card gets its texture and id. Pin its shape:
        cmp [rcx+0x130],0 ; jne go ; mov rax,[g_paramblk] ; cmp [rax+0x44],0 ; je ret
        ... mov rcx,[rax+0x3E8] ... call +0x39E560   (caption <- current sub-mission)
    and that slot 107 is the step 0xE entry that shows it."""
    c = offsets["constants"]
    vt = _find_vtable(img, ".?AVCMissionMacroAdventureEscape@@")
    fn = _slot(img, vt, int(c["ESCAPE_STEP4_SLOT"]))
    body = img[fn:fn + 0xC0]
    f130 = int(c["MACRO_INTRO_DONE"], 16)
    assert body[6:13] == b"\x83\xb9" + struct.pack("<I", f130) + b"\x00", "step 4 no longer tests macro+0x130 first"
    assert body[16] == 0x75, "expected jne after the +0x130 compare"
    assert body[18:21] == b"\x48\x8b\x05", "expected mov rax,[rip+g_paramblk]"
    rip = fn + 18 + 7 + struct.unpack_from("<i", body, 21)[0]
    assert rip == int(c["G_MISSION_PARAMBLK"], 16), "the gate no longer reads the mission param block"
    assert body[25:29] == b"\x83\x78" + bytes([int(c["MISSION_PARAMBLK_SETUP"], 16)]) + b"\x00", "gate field is not +0x44"
    assert body[29:31] == b"\x0f\x84", "expected je (early return) when +0x44 == 0"
    cap = b"\x48\x8b\x88" + struct.pack("<I", int(c["MAINMGR_START_CAPTION"], 16))
    i = body.find(cap)
    assert i != -1, "step 4 no longer loads [g_MainMgr+0x3E8]"
    j = body.find(b"\xe8", i + 7)          # skip the mov itself: 0x3E8 contains an E8 byte
    assert j != -1
    tgt = _thunk(img, fn + j + 5 + struct.unpack_from("<i", body, j + 1)[0])
    assert tgt == int(c["CAPTION_SET_CURRENT"], 16), f"step 4 calls +0x{tgt:X}, not the caption setter"
    # slot 107: the step 0xE entry. It is the extras-only caller of the show
    # wrapper +0xB4DF00 and then arms the 240+8 frame window (+0x164 = 8).
    entry = _slot(img, vt, int(c["ESCAPE_INTRO_ENTRY_SLOT"]))
    eb = img[entry:entry + 0x100]
    assert b"\xc7\x83\x64\x01\x00\x00\x08\x00\x00\x00" in eb or b"\xc7\x87\x64\x01\x00\x00\x08\x00\x00\x00" in eb or b"\xc7\x86\x64\x01\x00\x00\x08\x00\x00\x00" in eb, \
        "slot 107 no longer arms the 8-count window at +0x164"


# --- Speed King intro: the title card is gated on a save flag the PC never clears --

CAPTION_BIN = ROOT / "refs" / "caption_en.bin"   # data/scenario_en/caption.bin, 2024 rev


def _caption_entry(data, idx):
    """caption.bin: big-endian words, pointers are ABSOLUTE file offsets.
    Header +4 = count; entries at 0x10, stride 0x20: +0 cond ptr, +4 ncond,
    +8 act ptr, +0xC nact, +0x10 name ptr, +0x14 sub ptr, +0x18 id, +0x1C text.
    Programs are word lists; an op word has bit 31 set, its args follow."""
    be = lambda o: struct.unpack_from(">I", data, o)[0]
    count = be(4)
    assert idx < count
    e = 0x10 + idx * 0x20
    def prog(ptr, n):
        words = [be(ptr + 4 * i) for i in range(n)]
        ops, cur = [], None
        for w in words:
            if w & 0x80000000:
                cur = [w & 0x7FFFFFFF, []]; ops.append(cur)
            else:
                cur[1].append(w)
        return [(op, tuple(args)) for op, args in ops]
    name_ptr = be(e + 0x10)
    name = data[name_ptr:data.index(b"\0", name_ptr)].decode("ascii")
    return {
        "name": name, "id": be(e + 0x18),
        "cond": prog(be(e), be(e + 4)), "act": prog(be(e + 8), be(e + 0xC)),
    }


def test_speed_king_caption_is_gated_on_332_10_and_the_mod_clears_it():
    """Root cause of 'intro only on the first run per process' (2026-09-09).

    Entry 130 is the SPEED KING CHAMPIONSHIP card. Its condition is
        FlagGet(199,10)==1  AND  FlagGet(332,10)==0
    and its completion action sets 332:10 = 1. Nothing on PC clears 332:10
    (Bob's retail reset block would, but PC never reaches it), so the card
    shows once per process. ResetExtrasStartFlags must clear it with 199:10.
    """
    if not CAPTION_BIN.exists():
        pytest.skip("refs/caption_en.bin missing - copy data/scenario_en/caption.bin")
    data = CAPTION_BIN.read_bytes()
    e = _caption_entry(data, 130)
    assert e["name"] == "2d_mn_ch_dlc_bakuso_ch" and e["id"] == 102, e
    OP_FLAG_GET, OP_COMBINE, OP_FLAG_SET = 0x10, 0x33, 0x11   # matcher +0xCBB6C0 op ids (hex)
    assert e["cond"] == [(OP_FLAG_GET, (199, 10, 1)), (OP_COMBINE, (0,)),
                         (OP_FLAG_GET, (332, 10, 0))], e["cond"]
    assert e["act"] == [(OP_FLAG_SET, (332, 10, 1))], e["act"]

    # The mod's mode-end reset table must carry that row, and it must sit in the
    # SAME table as 199:10 so both clear in one FlagSetRaw-then-FlagSetEdge pass.
    src = (ROOT / "src" / "game.cpp").read_text(encoding="utf-8")
    table = src[src.index("const ExtraFlag kExtrasReset[]"):]
    table = table[:table.index("};")]
    rows = set(re.findall(r"\{\s*(\d+),\s*(\d+),", table))
    assert ("199", "10") in rows
    assert ("332", "10") in rows, "332:10 (Speed King caption-seen) missing from kExtrasReset"


def test_colo_scene_tail_clears_scn033_unconditionally(img, offsets):
    """AmebaFloor rests on one fact: PC's coliseum scene-flag function ends, on
    every path, by lowering STID_ST_TOUGI_SCN033 (alias 1242 -> 521:12) through
    the flag manager's alias table and the raw setter. PS3's twin keeps the flag
    set. Pin that tail so a game update that changes it fails here, not in play."""
    entry = int(offsets["patterns"]["COLO_SCENE"]["expect_rva"], 16)
    tail = entry + 0xF29                       # +0x393709
    alias = 1242 * 4                           # u32 (group|bit<<16) per alias
    want = (bytes.fromhex("44 0f b7 81") + struct.pack("<I", alias + 2)   # movzx r8d,[rcx+bit]
            + bytes.fromhex("45 33 c9")                                   # xor r9d,r9d  (value 0)
            + bytes.fromhex("0f b7 93") + struct.pack("<I", alias)        # movzx edx,[rbx+group]
            + bytes.fromhex("49 8b ca 48 83 c4 20 5b e9"))                # mov rcx,r10; epilogue; jmp
    assert img[tail:tail + len(want)] == want, img[tail:tail + len(want)].hex(" ")
    jmp = tail + len(want) - 1
    target = _thunk(img, jmp + 5 + struct.unpack_from("<i", img, jmp + 1)[0])
    assert target == int(offsets["patterns"]["FLAG_SET_RAW"]["expect_rva"], 16), f"0x{target:X}"
