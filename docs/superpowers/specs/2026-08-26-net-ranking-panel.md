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
