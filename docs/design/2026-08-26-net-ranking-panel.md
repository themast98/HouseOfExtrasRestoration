# The House of Extras recap: what it is, and what remains to build

## What it actually is

Video of the PS3 build running in an emulator with **no PSN connection** shows
the panel appearing normally at the end of a Battle King run:

```
            NETWORK RANKING
       -Battle King Ranking (Kiryu)-
    ------------------------------------
                  Score
    [ Latest Record ]              0
    [ Best Record   ]              0
```

It is a small **local** panel - your latest score and your best - not a
leaderboard of other players. The network supplied only the two numbers; with
no server they are simply 0, and the panel renders regardless.

An earlier conclusion in this project - that the recap was an online
leaderboard and therefore unrestorable - was **wrong**, and the video is what
disproved it. The structural argument behind it was also faulty: a scan for
PS3 step-0x2C handlers required a literal `li r4, <id>` within 12 instructions
of `OpenScreen`, which missed handlers with a wider gap. Widening the window
found a fourth (screen 227, queues step 0x2D).

## What survives on PC

* `pjs_net_ranking.csb`, `pjs_net_ranking.par` and `pjs_net_ranking_hires.par`
  all ship inside `data/2d/cse_*.par`.
* "NETWORK RANKING", "Score", "Latest Record", "Best Record" and the wreath are
  **artwork**, not code strings - they appear in neither binary, so they are
  baked into the layout and are present on PC.
* `LAYOUT_LOAD` (`sub_483AA0`) is generic and name-driven, with no whitelist.
* The sibling widget is intact: `NETRANK_CTOR`, `NETRANK_LOADLAYOUT`,
  `NETRANK_BIND`, `NETRANK_SHOW`, `NETRANK_DRAW` - a 0x28-byte panel whose int
  value lives at +0x20 and is drawn as `%d`.

## What was lost, and can be supplied

Three dynamic strings:

1. the board name, from the PS3 table - verbatim:
   `Survival King Ranking`, `Battle King Ranking (Akiyama|Saejima|Tanimura|Kiryu)`,
   `Fastest Killer Ranking (Akiyama|Saejima|Tanimura|Kiryu)`, `Speed King Ranking`
2. Latest Record - the run just finished (kill count, `+0x208`, confirmed)
3. Best Record - already tracked in `HouseOfExtras.records`

## Build sequence

1. `PushHeap(HEAP_ID_UI)`, allocate the 0x28 panel, `NETRANK_CTOR`.
2. Allocate a 0x128 layout and call `LAYOUT_LOAD(layout, "pjs_net_ranking", 0)`
   directly, bypassing `NETRANK_LOADLAYOUT` (which hardcodes the *minigame*
   layout name `pjs_mg_net_ranking`).
3. Store the layout in the panel, `NETRANK_BIND` once the csb has loaded.
4. Write the value at `+0x20`; `NETRANK_SHOW(group, 1)`; `NETRANK_DRAW` per frame.
5. Dismiss on X/O, as the PS3 did, then queue step 0x2E.

## Cautions carried forward

* Do NOT call the caption setter `sub_3A46D0` on anything but screen 220 - it
  writes a variant to `+0x1B8`, where other classes keep a pointer.
* Do NOT open screen 222 unless screen 217 is live; its constructor
  dereferences that slot without a null check.
* Screen field offsets are per-class; probing an unrecognised class is
  meaningless even where it is in bounds.


## The PS3 owner (found 2026-08-26)

Looking for the class that ORIGINALLY owned this layout - rather than modelling
on the surviving minigame look-alike - is what unstuck this.

Finding the reference took a corrected scanner: SN Systems forms the low half of
an address with `addic` (opcode 12), not just `addi`/`ori`. Every earlier xref
tool accepted only `addi`/`ori` and therefore reported ZERO references to
strings that are demonstrably referenced.

`pjs_net_ranking` (VA 0xEEA750) is referenced exactly once, at 0x006971DC:

```
0x697008   (function start)
  ...
  bl   0x41ca18            TransitionSetup(mission, 0, kFloat, .., 0)
  li   r3, 0xa
  bl   0x10604             PushHeap(10, 0)
  li   r3, 0x118
  bl   0x107ec             operator new(0x118)      # PC equivalent: 0x128
  lis  r4, 0xef
  addic r4, r4, -0x58b0    r4 = "pjs_net_ranking"
  li   r5, 0
  bl   0x191970            LoadLayout(mem, name, 0)
  stw  r3, 0x1b0(r31)      <<< this->[0x1B0] = layout
  ...
  bl   0x106a4             PopHeap()
```

**The layout goes in `+0x1B0` - the STANDARD layout field**, the same one
`CActionSurvivalCaption` uses. The owner is an ordinary action drawn by the
engine's normal machinery; it never had a bespoke widget. Reusing the minigame
widget was the wrong shape from the start, and its unguarded pane walk is what
crashed the game twice.

The owner lives in **screen slot 233** on PS3 (`mainMgr+0x580`, slot base 0x1DC,
4-byte pointers). The step handler that opens it is 0x0043EE14, and the
descriptor immediately after it is the matching waiter:

```
r4 = g_mainMgr[0x580]          ; slot 233
cmpwi r4, 0 ; bne skip
li r4, 0x16 ; bl SetNextStep   ; then step 0x16
```

PC screen id 233 is `CActFaceSpecialFileManager` - the ids are renumbered and
the net-ranking class is absent from PC's factory table, so there is nothing to
open directly.

## Revised plan: borrow a working owner

Since the owner is just "an action with a layout at +0x1B0", use one that still
exists and is known safe, rather than reconstructing one:

1. Load `pjs_net_ranking` ourselves (already proven to work).
2. Wait until `g_layoutRes[slot].count > 0`, then build pages once.
3. **Check page 0 and its element are non-null before anything touches them.**
4. `OpenScreen(mgr, 220, arg3)` - the caption action, repeatedly exercised
   without a crash - and wait for its `+0x1A8` to become 1 so its own load
   (which loops pages 0..3) has already finished on ITS layout.
5. Swap `screen[0x1B0]` to our layout.
6. `sub_3A46D0(screen, 0, hold)` - opens the draw gate and plays page 0's
   in-animation on our layout.

Every dereference in that path is checkable in advance, which the previous two
attempts were not.


## Live results (2026-08-26): builds cleanly, does not rasterise

First run that neither crashed nor hung. The whole lifecycle works:

```
netrank: layout=... slot=16 ('pjs_net_ranking')
netrank: built after 2 frames, 17 pages, all elements present
netrank: page 0 unsuppressed
netrank: released layout ... (slot 16)     <- teardown clean, game survived
```

17 pages confirms the crash diagnosis exactly: that count is what overflowed
the 0x28-byte panel of the minigame widget.

**Ruled out live, with the panel frozen on screen:**

* Not the fade. State byte 0 and alpha 0.0 - no overlay at all this time.
* Not the renderer being idle. The d-pad element draws in the same frame.
* Not `elem[0x68]` / `sub_48BB90`. Every element in the global list has
  `+0x68 == NULL`, **including ones that are visibly drawing**, so it was never
  the discriminator.
* Not element flags. Live drawing elements read `0x51`/`0x71` while ours read
  `0x4B`; setting ours to `0x51` changed nothing.
* Not page selection. Unsuppressing ALL 17 pages and flagging every element
  `0x51` still rendered nothing.
* Not the element list. 236 elements registered, ours among them.

So the elements are registered, unsuppressed and flagged exactly like elements
that do draw, and still produce no pixels. The remaining candidates are below
the element: the pane tree, its textures, or its geometry.

`g_layoutTexPar[16]` is NULL for our layout - but that is true of every live
layout including drawing ones, so it is suggestive at best, not evidence.

**Next avenues, in order of expected value:**

1. Compare our element's PANE TREE (`elem+0x30`) against a drawing element's -
   pane count, flags, and whether any pane has a resolved texture handle.
2. Find what the render walk (`sub_488E00` -> `sub_48BC70` -> `sub_48C050`)
   actually requires of a pane before it emits geometry, and check ours against
   it rather than inferring from element flags.
3. Only then consider whether `pjs_net_ranking.par` needs an explicit texture
   load, using the single call site of `sub_482780` as the model.


## Phase 1 (2026-08-26): the element level is CLEARED

### 1a. The collect filter, read from the consumer

`sub_488E00` walks the list at `0x1980D70` in three passes, following
`elem+0x8` as the next pointer. The general pass is:

```asm
movzx eax, byte [rbx+0x2b] ; sub al,2 ; cmp al,1 ; jbe skip   ; 0x2b must not be 2 or 3
test  byte [rbx+0x2c], 1   ; je skip                          ; bit0 must be set
mov   rcx, rbx ; call sub_48A210                              ; == return elem[0xBC]
test  eax, eax ; je collect                                   ; suppress 0 -> COLLECT
test  dword [rbx+0x2c], 0x400 ; je skip                       ; only for SUPPRESSED ones
mov   rbx, [rbx+8]                                            ; next
```

Our element has `0x2b = 0`, bit0 set and `+0xBC = 0`, so it satisfies every
condition and IS collected. Bit `0x400` only matters for suppressed elements,
so it is irrelevant here despite looking important at first.

**Therefore the failure is strictly downstream of collection, in rasterising
the pane tree.** That is established by reading the consumer, not inferred from
a sibling - which is the mistake that produced the previous three wrong
theories.

Note: an earlier claim that `sub_48C050` "is only 0x20 bytes and is NOT the
rasteriser" was itself wrong, and wrong for an instructive reason - see
"Reading chunked functions" in the 2026-08-27 section. It is a 0x147-byte
function and it IS the recursive pane walker.

### 1b. Reference: what a DRAWING pane looks like

Captured live from three elements that were visibly drawing (pause menu),
saved to `refs/drawing_pane_reference.json`. Consistent shape:

| offset | value | note |
|---|---|---|
| +0x000 | `0x18003F` / `0x3F` | flags; low 6 bits always set |
| +0x008 | pointer | non-null |
| +0x010 | pointer | non-null |
| +0x018 | float ~22000-23500 | a running time |
| +0x058 | `0x7B` | constant across all three |
| +0x060, +0x068 | `1.0f` | scale/alpha |
| +0x070, +0x078 | `-1` | |
| +0x098 | pointer | non-null |
| +0x0A0 | pointer or 0 | sibling |

### Phase 2 (needs one frozen run)

Diff our pane root (`elem+0x30`) against that table, read-only. Expected
discriminators, in order: a null `+0x008`/`+0x010`/`+0x098`, low flag bits
clear at `+0x000`, or zero scale/alpha at `+0x060`/`+0x068`.

---

# Session of 2026-08-27: the element level is now provably identical to a drawing element, and it still does not rasterise

This session ran almost entirely against a **live frozen recap** driven through
the `HouseOfExtras.cmd` console, so nearly every statement below is a
measurement rather than a deduction. Where something is inferred it says so.

## The one real bug found and fixed

`elem+0x2c` bit `0x02` **pauses the element's animation clock**. The forward
advance in `sub_48BC70` opens with:

```asm
+0x1F8   test al, 2        ; al = elem+0x2c
+0x1FA   jne  .skip        ; bit 0x02 set -> elem+0x38 never advances
```

Bit `0x02` is set by the page constructor `sub_484260+0x5B`, on the **same
branch** that calls `SetSuppress(elem, 1)` - the branch taken when its 4th
argument is 0. So "created suppressed" and "animation paused" are one decision,
and clearing the suppress flag is only half of "show it".

Measured: `elem+0x38` stayed at exactly `0.0` for sixteen minutes of frozen
recap. Poking it above zero started it advancing immediately, and it propagated
into every pane in the tree (`pane+0x18`), confirming both the gate and that the
whole walk runs on our element every frame.

Every engine element that is visibly drawing has bit `0x02` clear:
`pjs_btl 0x451`, `pjs_kill_counter 0x51`, `pjs_life 0x51`. Ours read `0x4B`.

**Fixed** in `src/netrank.cpp` as `Unpause()`, called from `Show()` next to
`SetSuppress(elem, 0)`. It clears the bit and, because the advance is also
skipped while the time is exactly `0.0`, starts the clock at `1.0`. Verified
live: after clearing it our element became `0x51` - the engine itself then
cleared `0x08` and set `0x10` on the next frame (`sub_48BC70+0x257`/`+0x26D`) -
i.e. **byte-identical in `+0x2c` to `pjs_life` and `pjs_kill_counter`**, with a
freely running clock. The panel still did not appear.

## Method note: the positive control

The single most valuable move of the session came from the user: *"why don't you
check the dpad? its the only visible element"*.

Mid-session I had convinced myself that dark grey d-pad glyph was part of the
RTSS overlay, and therefore that **nothing** the game drew was visible - which
would have meant the panel was never the problem. That was wrong, and it cost
real time. Identifying it properly gave the control this investigation had
lacked from the start: a *known-visible element in the same frame, in the same
pools*, to diff against.

Its element turned out to be `pjs_btl` (slot 19) / `pjs_kill_counter` (slot 17).

**Carry forward: never reason about "is anything rendering" from a screenshot
alone. Establish a known-good element first, then diff against it.**

## Reading chunked functions (tooling correction)

Two separate xref/disassembly failures this session, both silent:

1. **MSVC routes every call through the incremental-link table.** A byte scan
   for `E8` targeting a function finds *nothing*; the real call sites target a
   5-byte `E9` thunk. Resolve function -> thunks -> callers. Before fixing this,
   `sub_4866C0` and friends appeared to have **zero** callers.
2. **The `.pdata` function table lists continuation chunks as separate
   entries.** `sub_48C070` looked like an unreferenced function; it is a chunk
   of `sub_48C050`, resolvable through the `UNWIND_INFO` CHAININFO flag (0x4)
   in the unwind byte's high 5 bits. This is what produced the bogus
   "`sub_48C050` is 0x20 bytes" claim.

Both are fixed in `scratchpad/tools/xref2.py` (thunk-aware callers) and
`scratchpad/tools/dis2.py` (chunk-following disassembly with named call
targets). **Use these, not the older `callers.py` / `disx.py`.**

## The render path, now fully mapped

```
sub_488E00(pass)                  global element list at 0x1980D70
  |  collect: elem[0x2b] not in {2,3}, elem[0x2c] bit0,
  |           (elem[0xBC]==0 || elem[0x2c] & 0x400)
  |  into the array at [0x1980D60], capacity [0x1980D68] (=512)
  |  sub_486F70   sort
  +-> sub_48BC70(elem)            per collected element
        |  bail if !(elem[0x2c] & 1) or elem[0x10] == NULL
        |  elem[0x10] == &g_layoutRes[slot]   <- also published to 0x1980CE8
        |  clamp elem+0x38, local time = max(0, t - seqStart)
        |  draw iff elem[0xBC]==0 or elem[0x2c] & 0x400
        +-> sub_48C050(elem, pane, time)      RECURSIVE pane walk
              |  sub_486920(pane)             per-pane work
              |     save/restore render state; maintains the +0x58 cache mask
              |     sub_485840(pane, RS, stack)   composes colour/transform
              |     draw gate:
              |         test al, 0x60          pane flags 0x20|0x40 -> container
              |         test [r15+0x30], 0xff000000   composed ALPHA
              |         bt eax, 0x12           bit 18 = force-draw override
              |     sub_4889C0(cursor, pane+0x10)   emit quad from (x,y,w,h)
              |  descend  pane+0xA0 (child), pane+0xA8 (sibling)
```

### Structures confirmed live

| thing | fact |
|---|---|
| `elem+0x00` / `+0x08` | prev / next (the list is **doubly** linked) |
| `elem+0x10` | `&g_layoutRes[slot]` - subtract the base, divide by 0x70, get the slot |
| `elem+0x20` | csb header; `byte+0x05` = sequence count, `+0x40` = table |
| `elem+0x28` | sequence index; `word[table + idx*2]` = start frame |
| `elem+0x2c` | `0x02` paused, `0x400` draw-while-suppressed |
| `elem+0x38` / `+0x3C` | animation frame counter / speed; clamp max 5,184,000 = 24h at 60fps |
| `elem+0xBC` | suppress |
| page record | **0x30 bytes**, inline array at `layout+0x110`; `+0x08` slot, `+0x10` element |
| pane | **0xB0 bytes**, contiguous; `+0x18` time, `+0x4C`, `+0xA0` child, `+0xA8` sibling |
| `g_layoutTexPar` | `0x1980A80`, **stride 8**, indexed by slot |

### Corrections to the Phase 1/2 reference table

* `pane+0x58 == 0x7B` is **not** a health signal. `sub_486920` uses `+0x58` as a
  *"which render-state fields have been cached"* mask; each bit pairs with one of
  `+0x5C/+0x60/+0x64/+0x68/+0x6C`, saved on first visit and restored after.
  Every processed pane reaches `0x7B`, ours included.
* `pane+0x18` is the **animation time**, fed from `elem+0x38`. The reference
  values of ~22000-23500 were simply long-lived elements, not a magic constant.
* The original Phase 2 dump showed `+0x58 = 0` and `+0x18 = 0` only because it
  was taken on the same frame as the unsuppress, **before** the render pass ran.
  Re-reading later showed `0x7B` and a running clock. **Sample diagnostics at
  least one frame after the change that is supposed to affect them.**

## Ruled out this session, each with live evidence

* **Collection** - our 17 page elements sit in the engine's collected array at
  indices 11-28, interleaved with the 11 engine elements collected that frame.
* **The pane walk** - both root and children reach `+0x58 = 0x7B`; the poked
  clock propagates to every leaf. The tree is walked in full, every frame.
* **Pane draw eligibility** - leaves are `0x400001F`: `0x20`/`0x40` clear, so
  they are geometry-emitting panes, and `0x4000000` is the engine's own
  "visited" marker from `sub_486920+0x266`.
* **The alpha gate** - forced bit 18 (`0x40000`) open on all 11 leaves. Nothing.
  Note this bypasses the *skip* but not the vertex colour, so it is not a full
  alpha disproof on its own - however the *visible* control pane has
  `+0x6C = 0x00FFFFFF` while ours is `0xFFFFFFFF`, i.e. ours is the more opaque
  of the two.
* **Geometry** - sprite records are valid (`w`,`h` int16 at `+0x14`/`+0x16`,
  float `x`,`y` at `+0x18`/`+0x1C`). Enlarging all 11 leaves to 1900x1000 at the
  origin produced **zero** lit pixels.
* **Textures** - `g_layoutTexPar[slot]` is NULL for **all 21** live layouts,
  including ones that draw. `sub_482780` has exactly one call site
  (`sub_4877A2+0x44`) and nothing in this build reaches it.
* **`LoadLayout`'s 3rd argument** - it only chooses whether the *name* is
  forwarded to `sub_487860`. Passing 0 forwards it, so we take the richer path.
* **Page selection** - all 17 pages unsuppressed with running clocks: nothing.
* **Animation state** - swept frames 1/15/30/60/120/300 with speed frozen at 0,
  and sequence index 0/1/2 at two times each. All zero lit pixels.
* **The fade overlay** - fade block at RVA `0x198C620`: current alpha
  `0x198C630 = 0.0`, target `0x198C638 = 0.0`, state byte `0x198C624 = 0`.
  Fully transparent, so nothing is covering the screen.
  (`TransitionSetup` mode 0 or 1 targets alpha **1.0 = opaque**, from
  `0x1209ABC`; any other mode targets 0. We pass mode 1 at `recap.cpp:298`.)

## Where it stands

The panel is loaded, built, collected, walked, animated and draw-eligible, in a
state **indistinguishable in every field measured** from engine elements that
are visibly drawing in the same frame - and it puts no pixels on screen.

## Next steps, in order of expected value

1. **Pane flags bits 19 and 20.** The visible control pane reads `0x0018003F`;
   ours reads `0x0000003F`. Those two bits are exactly what `sub_485840` - the
   colour/transform composer - tests at `+0x2F` (`0x80000`) and `+0x84`
   (`0x100000`). No code in the layout module sets them, so they are authored in
   the csb. **Read `sub_485840` fully** and determine what a pane without them
   ends up with. If it means "no colour track, inherit the parent" then this is
   a dead end - but it is the largest unexplained difference on the table.
2. **`pane+0x4C`.** Visible `0xA2`, ours `0`. Written by `sub_48C050+0x57` from
   `elem+0x4e`, but only when `elem+0x4e != 0xFFFF`; ours is `0xFFFF` because
   `sub_48BC70+0xFE` writes `0xFFFF` there after every draw. The non-`0xFFFF`
   value comes from the `elem[0x2c] & 0x100` path at `sub_48BC70+0x15A`, via
   `sub_485EA0` and `sub_C5B910`. **Find out what sets bit `0x100`** - it may be
   the "this element has real content to draw" bit we are missing.
3. **Parse the actual `.csb`.** Nothing in this project has ever inspected the
   file. Pull `pjs_net_ranking.csb` out of `data/2d/cse_*.par` (SLLZ v1 decoder
   is in `tools/sllz.py`) and read page 0's real structure. This would settle
   whether page 0 is even the visible panel, and what its panes are authored to
   do. Currently the single largest blind spot, and the only step that does not
   depend on guessing from the running process.
4. **Only then** consider the `OpenScreen(220)` + `screen[0x1B0]` swap from the
   "borrow a working owner" plan earlier in this document. It remains the most
   faithful route to what the PS3 did, and it side-steps every unknown by letting
   a real engine action own the layout - but it is also the riskiest, so it
   should follow 1-3.

## Housekeeping

* `HouseOfExtras.ini` left at `RecapFreeze=0` (playable) and
  `NetRankingPanel=1`. Set `RecapFreeze=1` to resume console-driven debugging.
* The console (`tools/gcmd.py`) is the right tool for this work: `read`,
  `readrva`, `poke32`, `poke64`, `slots`, `base`, `resume`. **`read` is capped at
  512 bytes per command**, but many commands can be batched into one call.
* `tools/shot.ps1` is DPI-aware and captures the client rect at true 1920x1080.
  Scoring a capture with PIL for "lit pixels below y=140" (excluding the RTSS
  overlay band) turns a visual check into an automated one - worth keeping.
* The game must be launched through Steam (`steam://rungameid/1105500`);
  launching the exe directly crashes on SteamStub.
