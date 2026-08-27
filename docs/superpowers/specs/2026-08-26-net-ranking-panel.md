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
