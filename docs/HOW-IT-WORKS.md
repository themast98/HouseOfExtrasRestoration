# How the House of Extras Restoration works

This document explains what the mod does inside the game and how the code is put
together. It is written for someone who wants to read, build or modify the source,
and assumes basic familiarity with x86-64, DLL injection and the idea of a
"mission macro" in the Yakuza engine (a class whose numbered *steps* run once per
frame until one of them queues the next step).

Everything here was established by reverse engineering the Steam build of Yakuza 4
Remastered (Yakuza4.exe, 2024-07-21, 28,396,336 bytes) against the decrypted PS3
EBOOT, where the missing code still exists. Source comments carry the primary
evidence; this document is the map.

---

## 1. What is broken on PC, and why

When Sega ported Yakuza 4 to PC they removed the `src/ranking` directory of the
game's source tree: the online NETWORK RANKING feature that Bob Utsunomiya's House of
Extras modes fed into. That removal did not simply turn the network off. It took
code paths that the modes depend on with it, and the port shipped with the modes
in the following state:

| Symptom on PC | Root cause |
|---|---|
| Battle King / Fastest Killer end on an infinite black screen | `CMissionMacroColosseumExtra` steps 0x2C and 0x2D were gutted to `ret 0`. A step handler that never queues a next step runs forever. |
| Only Battle King appears in Bob's menu | Four rows are gated on save flags that the game copies from DLC entitlements. The PC entitlement table grants bits 0x00–0x16 and 0x1A and silently skips 0x17–0x19. |
| A completed mode cannot be replayed | Bob's post-match flag reset (23 flag writes) lived in the deleted step path. |
| Endless Survival Tag ends on a black screen | Its step 0x21 hands off to mission 143 "Ranking Display", which no longer exists, and that arm skips the teardown step. |
| No NETWORK RANKING panel after any mode | The panel's layout and artwork still ship (`pjs_net_ranking`), but the code that built it and the three strings it showed were in `src/ranking`. |
| Speed King's title card and countdown only play on the first run per process | A once-per-object latch on the chase action. PS3 rebuilds the action each run, PC reuses it. |
| The Ameba logo is missing from the coliseum floor in Battle King / Fastest Killer | PC's coliseum scene-flag function clears scene object SCN033 unconditionally in its tail. PS3 leaves it set. |
| Bob says nothing after a mode ends | The post-mode talk entries in his stage script and the paired message flows were removed from the PC data. |
| The survival-tag Controls panel is 1.5x too large | It sizes itself from the output resolution instead of the fixed 1280x720 2D canvas. |
| Chase stamina drains twice as fast as on PS3 | The drain was tuned for PS3's 30 Hz logic tick and runs at 60 Hz on PC. |
| The game crashes seconds after launch with the Steam overlay | An unrelated bug in the stock game: an SSE strlen reads past a page boundary in the achievement callback. Proven present with no mods installed. |

The mod fixes every row of that table. Some need code, some need data, and the
rest of this document follows that split.

---

## 2. The two halves

The release is a single folder under `mods/`:

```
mods/House of Extras Restoration/
    mod-meta.yaml                 SRMM metadata (name, author, version)
    HouseOfExtras.asi             the plugin (this repository)
    2d/cse_en/...                 ranking-panel layouts and the kill-counter layout
    font_hd/ranking/...           the ranking number font
    stage/ST_TOUGI/ARCMN/...      coliseum texture archives with the Ameba floor
    wdr_par_en/wdr/msg/...        Bob's post-mode message flows
    wdr_par_en/wdr/pac/...        Bob's stage script with the post-mode talk entries
```

**The data half** is loaded by Shin Ryu Mod Manager (SRMM) and its Parless loader
exactly like any other data mod: SRMM lists the files in `YakuzaParless.mlo`, and
Parless redirects the game's file opens to the mod copies.

**The code half** is `HouseOfExtras.asi`, a plain native DLL. Parless has a mod
script loader: after it reads the mod load order it walks every listed file and
`LoadLibrary`s any that ends in `.asi`. SRMM lists every root-level file of a mod,
so putting the ASI at the mod root is all it takes. There is no library, no
registry entry and no .NET host involved. Ticking the mod off in SRMM therefore
disables the code too.

Every file the plugin creates at runtime (`HouseOfExtras.ini`, `.log`, `.records`)
is resolved relative to the DLL itself (`paths.cpp`, `ModRelative`), so it lands in
the same folder. Deleting the folder removes the mod completely.

---

## 3. Lifecycle of the plugin

`dllmain.cpp` is the entry point and reads top to bottom as the boot sequence:

1. **DllMain** only starts a thread. Parless loads us from inside its own DllMain,
   under the Windows loader lock, so nothing heavier is allowed there.
2. **Init thread** opens the log and reads `HouseOfExtras.ini` (written with
   documented defaults on the first run).
3. **Wait for the code to exist.** Yakuza4.exe is SteamStub-encrypted: `.text` is
   unreadable on disk and is decrypted in memory a moment after launch.
   `resolve::WaitForText` polls for the anchor pattern every 250 ms, up to two
   minutes, before anything is touched.
4. **The Steam crash fix installs first**, before the full resolve. It races a
   Steam callback that fires about four seconds after launch, and it needs nothing
   but a decrypted `.text`.
5. **Resolve** every address (section 4). If anything mandatory fails, the plugin
   logs it and installs nothing. A resolve failure is a safe outcome by design.
6. **Bind** the engine function pointers (`game::Bind`) and install the hooks and
   patches that the ini enables (section 5).
7. **Heartbeat thread.** Every two seconds it re-applies the mode unlock. The flag
   manager is replaced wholesale on every save load, so a one-shot unlock would be
   silently undone.

The 2D draw hook installed in step 6 is also the only per-frame callback we own on
the *game thread*. Anything that must run there every frame (the mode-end observer,
the console channel, the panel upkeep) hangs off it rather than off the heartbeat.

---

## 4. Finding addresses without hardcoding them

Nothing outside `resolve.cpp` may use an absolute address. `offsets.json` is the
single source of truth and `tools/gen_offsets.py` turns it into `src/offsets.h`.
The generated header carries only the *inputs* to resolution; the absolute RVAs in
the JSON become `EXPECT_*` constants that are logged next to what was actually found,
so a game update shows up as a mismatch line rather than a crash.

Four techniques, in the order the resolver uses them:

- **One anchor pattern.** `CMissionMacroColosseum`'s intact step 0x2C handler is
  found by an AOB scan. Its body contains `call rel32` and RIP-relative operands for
  everything the recap needs: the fade-transition setup, the flag commit, the
  screen opener, the step setter, and the globals for the flag manager and the main
  manager. Each is read out of the anchor at a known instruction offset and
  followed through MSVC's incremental-link thunks (`E9 rel32`, up to four hops).
- **RTTI.** Vtables are located by their `.?AVCMissionMacroColosseumExtra@@`
  type-descriptor strings and Complete Object Locators. Slot indices then give the
  gutted step stubs (slots 62 and 63), the reference handlers in the sibling class,
  the `CActionColosseumExtra` vtable used to type-check a polymorphic pointer, and
  the `CMissionMacroAdventureEscape` / `AdventureSurvive` slots the other modes hook.
- **Secondary anchors and patterns.** The DLC-to-flag bridge yields the flag alias
  setter and the ownership bitset; the mission tick yields the mission manager;
  short unique patterns give the layout, heap, font, sound and flag APIs.
- **Verification.** Every hit is checked against its expected RVA and, for patch
  sites, against the exact bytes we are about to replace (a `ret 0` stub with its
  `int3` padding, a 16-byte prologue with no RIP-relative instruction, and so on).

`tests/test_offsets.py` runs the same checks offline against a decrypted memory dump
of the executable, so an offsets change can be validated without launching the
game. The dump is game code and is never committed (see `refs/README.md`).

---

## 5. Patching techniques used

`patch.h` has one primitive, a reversible `Patch` that records the original bytes.
Everything else is a pattern of use:

| Technique | Where | Why that shape |
|---|---|---|
| **Absolute jump into a gutted stub** (`mov rax, imm64; jmp rax`, 12 of 16 bytes) | steps 0x2C / 0x2D of ColosseumExtra | The stub is `ret 0` plus padding; the DLL is too far from the image for a rel32. |
| **Vtable slot replacement** | Speed King (AdventureEscape slot 88), Endless (AdventureSurvive slots 62/63) | The function at that slot is *shared* by eight classes because MSVC folded the deleted override into the base. Patching the function would hit every class; patching the slot hits one. |
| **Post-detour trampoline** (16-byte prologue copied, `jmp [rip+0]` both ways) | Ameba floor (`ameba.cpp`), talk diagnostics | The original must run unchanged and we only add to its result. |
| **Single-byte / immediate patches** | Endless handoff (one byte reroutes step 0x21), chase drain rate (NOP an ability test), tag help panel (four 17-byte call sites), font bank style | Smallest change that produces PS3 behaviour; each site's bytes are verified first. |
| **Near stub for a rel32 call** | Steam achievement fix | The call site is a `call rel32`; the replacement is placed in a page allocated within 2 GB of the image, RW first and then flipped to RX. |
| **Flag writes through the engine's own API** | mode unlock, post-match reset, post-mode Bob flags, Ameba scene flags | Aliases are decoded through the flag manager's definition table so a renumbered build stays correct. |

Every patch is gated by an ini key and logs a one-line summary of what it did.

---

## 6. The NETWORK RANKING panel

### 6.1 What it is

PS3 footage from an emulator with no PSN connection shows the end-of-mode panel:

```
        NETWORK RANKING
   -Battle King Ranking (Kiryu)-
        Score
   Latest Record        123
   Best Record          456
```

It is a small **local** panel. The network only ever supplied the two numbers.
All of its artwork survives on PC inside `pjs_net_ranking`; the deleted code built
the layout, wrote the board name and the two values, and waited for the player.

### 6.2 The step machine (`recap.cpp`)

The gutted step 0x2C handler is replaced by `HoE_Step2C`, which does its work once
and hands over; `HoE_Step2D` then runs every frame until the player closes the
panel. That is the same two-step shape the engine's intact sibling uses and the
same shape PS3's handler had: configure, then poll with no timer.

```
Step 0x2C   read kills / time from the mode's own objects
            update HouseOfExtras.records (best per board)
            netrank::Open()   allocate and load the pjs_net_ranking layout
            queue step 0x2D

Step 0x2D   until netrank::Ready(): wait (bounded)
            netrank::Show(latest, best) once
            per frame: netrank::Draw(), input edges, optional watchdog
            on confirm: Finish()
```

`Finish` is the single exit for every route, including error routes: it calls the
mode teardown virtual (`CMissionMacroColosseumExtra` slot 10), which the deleted
step 0x2D used to call and whose absence is what left the world state broken after
a mode. That call was recovered by reading PS3's step handler, where the class is
intact: `vfunc(kind); SetNextStep(0)` rather than the sibling's `SetNextStep(0x2E)`.

### 6.3 Building and drawing the layout (`netrank.cpp`)

Two things were learned the hard way and are documented at the top of the file:

- **The surviving minigame widget cannot be reused.** Its bind function loops over
  *all* pages of a layout and stores each into a 0x28-byte panel. The minigame
  layout has 3 pages; `pjs_net_ranking` has 17. Reusing it wrote 0x90 bytes into a
  0x28 allocation and crashed later, elsewhere.
- **Nothing needs to own a layout to draw it.** The render pass walks a global
  intrusive list of layout *elements*. Every element the page builder creates is
  on that list and is drawn when its pass byte selects 2D, its "armed" bit is set
  and its suppress byte is zero. So after building the pages, showing the panel is
  a matter of clearing the suppress byte on the pages we want.

The remaining problems were all about *why a correctly built layout put no pixels
on screen*, and each has its own fix:

- **Buried under a black quad.** The fade system draws a fullscreen black quad at
  sort key 0x3000 during the step our panel opens in. Elements sort by a key at
  +0x4C; the panel's is biased to 0x1000 so it composites above the fade.
- **Texture residency gate.** The element draw path discards a fully transformed
  quad if the texture id at pane+0x4C is not resident. Owning screens stamp a real
  id; a bare layout never did. The panel stamps it.
- **Dynamic text.** The three strings the deleted code drew are rendered through the
  engine's text API onto specific pages in specific bind modes. Which page and
  mode each string belongs on was the deleted owner's knowledge; it was found on
  screen with the console channel and then baked in as defaults.
- **Numbers.** PS3 draws the values with a dedicated 16-column ASCII number font
  (`ranking_hankaku.dds`) that the English font archive on PC does not mount. The
  mod ships the face and the plugin selects the bank; the count-up ("roll") takes
  30 frames, the LATEST value pulses white to cyan once, and page 9 carries the
  NEW RECORD banner on a new best. All timings are the PS3 constants.
- **Sounds.** Roll, settle, new-record and decide cues play through the engine's
  sound API with the PS3 cue keys.

### 6.4 Input

Pad state is read directly from the pad manager (`input.cpp`), never through an
engine call: by the time a step handler runs, the pad has already been sampled for
the frame and the values are immutable. The confirm button must be *released* before
the panel arms, so the next edge can only be a fresh press; both the rising and the
falling edge are then accepted, which makes exactly one event per press without
depending on an engine mode word whose writer was never found.

---

## 7. Per-mode wiring

Four boards, four different places the engine keeps the score:

| Mode | Where the value lives | How the panel is reached |
|---|---|---|
| Battle King | `CActionColosseumExtra` +0x208 kills (after type-checking the polymorphic slot at mainMgr+0xBA0) | ColosseumExtra steps 0x2C/0x2D |
| Fastest Killer | the action's own `CActionTimer` at +0x1A8, frames at +0x1B0, converted to PS3's `mm:ss'cc` composite | same steps; +0x1B8 selects the time layout |
| Speed King (Extra Chase) | a `CActionTimer` hanging off the main manager at +0x7E8, capped at 0x57E3F frames | `AdventureEscape` vtable slot 88, hooked only for the ranking chase (flag 199:10) and only once the run has actually finished; the return value is the control flow |
| Endless Survival Tag | the survival-tag manager at mainMgr+0x888: `score = [+0x1C]*37 + [+0x18]`, reproduced rather than called so both dereferences can be null-checked | `AdventureSurvive` step 0x2D, with the panel inserted where PS3's separate ranking mission ran; step 0x2C is hooked to suppress the mode's own result screen, as PS3 showed only the panel |

Win or loss is read from the game's own CLEAR-vs-FAIL decision for the running
mission, not inferred. An earlier build read two ints out of the wrong object and
defaulted to "won", which is how a lost Speed King run once wrote a record.

---

## 8. Save flags

The engine keeps save state as (group, bit) flags behind a manager with a
definition table, and most House of Extras behaviour is flag driven:

- **Mode unlock.** The plugin sets the three missing entitlement bits in the DLC
  ownership bitset *and* the three alias flags they map to. Both are needed: the
  game's surviving bridge writes the flags *from* ownership, so if ownership stayed
  false the bridge would clear the flags again.
- **Replayability.** Bob's post-match reset block (23 flag writes, recovered from
  the retail script) is replayed after a mode ends so the mode can be started again.
  It is armed on mode end and fired once the mission manager reports idle.
- **Bob's post-mode lines.** Two flags select the "done already" / "enjoy" flows.
  The talk entries in `pac_STID_ST_URANAI.bin` pair with the message flows in
  `uid00ee05b6.msg` **by index**, which is why the shipped `.msg` has its flow table
  rotated to match the PS3 order: with the wrong pairing the box opened empty.
- **Speed King intro every run.** The card is current while 199:10 is set and
  332:10 is clear, and completing it sets 332:10. Retail cleared 332:10 from Bob's
  reset block; the plugin clears it the moment the card has set it, and separately
  clears the once-per-object latch bit on the chase action (+0x782 bit 0x20) that
  PS3 avoided by rebuilding the action each run.
- **Ameba floor.** Scene objects show when their named flag is set. PC's coliseum
  scene function clears SCN033 (521:12) in an unconditional tail. `ameba.cpp` runs
  the original and then, if the stage is the coliseum and a ranking mode is armed
  (any of 199:2..9), raises SCN033 and lowers SCN003 through the same alias table.

---

## 9. Files the plugin writes

All next to the ASI in the mod folder:

- `HouseOfExtras.ini` — every fix is a switch, with the PS3 behaviour as default.
  `Diag*` keys turn on bounded logging and are off in the release. Keys are
  documented in the file itself; every documented key is also read in `config.cpp`
  (a key that is written but never read is a bug there).
- `HouseOfExtras.log` — one line per event, timestamped. Startup prints the version,
  every resolved address next to its expected value, and every patch applied.
- `HouseOfExtras.records` — `mode=value` per line, best per board. Values outside
  a plausible range (score > 9999, time > 99:59'59"99) are rejected on both load
  and store, so a bad read can never become an unbeatable best.
- `HouseOfExtras.cmd` / `.reply` — a developer command channel, polled from the
  game thread about four times a second, used with `tools/gcmd.py` to read memory,
  poke fields and call engine functions while the panel is up. Every address is
  checked with `VirtualQuery` first. Reads work from any thread; mutation is
  refused unless the pump runs on the game thread.

---

## 10. The data half in detail

| Path | What it restores |
|---|---|
| `2d/cse_en/pjs_mg_net_ranking.{csb,par,_hires.par}` | The PS3 version of the ranking layout (pane sizes and UVs differ from PC in 53 bytes) and its textures. |
| `2d/cse_en/pjs_kill_counter{,_hires}.par` | The kill-counter layout textures used by Battle King's HUD. |
| `font_hd/ranking/ranking_hankaku.dds` | The 512x256 number face the panel draws its values with. The English font archive does not mount a `ranking/` folder. |
| `stage/ST_TOUGI/ARCMN/st_tougi_arcmn_{day,ngt}_hires.par` | Whole coliseum texture archives with `s_sud_tou_df000.dds` (the Ameba decal) inside. The engine opens the archive whole and builds its texture pool from it, so a loose single-file override does not work. |
| `wdr_par_en/wdr/pac/pac_STID_ST_URANAI.bin` | Bob's stage script with the post-mode talk entries restored (PC shipped a terminator entry in slot 0). |
| `wdr_par_en/wdr/msg/uid00ee05b6.msg` | The paired message flows, rotated to the PS3 order so entry *i* meets flow *i*. |

Only the English data (`cse_en`, `wdr_par_en`) is covered.

---

## 11. Source map

| File | Role |
|---|---|
| `dllmain.cpp` | boot sequence, heartbeat thread, version string |
| `resolve.{h,cpp}` | runtime address discovery and verification |
| `offsets.json`, `tools/gen_offsets.py`, `src/offsets.h` | the offset table and its generated header |
| `patch.{h,cpp}` | reversible byte patches, module base and size |
| `paths.{h,cpp}` | module-relative file paths |
| `config.{h,cpp}` | ini defaults, reading and the default-file writer |
| `log.cpp` | the log file |
| `game.{h,cpp}` | engine bindings, score readers, flag work, the smaller fixes (mode unlock, reset block, Endless handoff, chase drain, tag help panel, Speed King intro) |
| `recap.{cpp}` | the step handlers for all four boards and the single `Finish` exit |
| `netrank.{h,cpp}` | building, showing, drawing and tearing down the ranking panel |
| `records.{h,cpp}` | the best-score file |
| `input.{h,cpp}` | pad state and confirm edges |
| `console.{h,cpp}` | the developer command channel |
| `steamfix.{h,cpp}` | the stock-game achievement crash workaround |
| `ameba.{h,cpp}` | the coliseum floor detour |
| `talkdiag.{h,cpp}`, `probe.{h,cpp}` | diagnostics, off in the release, kept because they were how the answers were found |
| `tests/` | offline verification against a decrypted dump (`test_offsets.py`) and the SLLZ decoder (`test_sllz.py`) |
| `tools/` | build/deploy, offset generation, and the reverse-engineering helpers used during development (call-site finder, gutted-virtual finder, signature generator, PS3 cross-referencer, SLLZ decoder, screenshot and log watchers) |

---

## 12. Working on it

- **Build**: `build.sh` (mingw-w64 GCC, static, C++17). `tools/deploy.sh` builds,
  runs the tests, and copies the ASI into the installed mod folder, refusing while
  the game runs or if a stray root-level ASI exists.
- **Add an address**: put it in `offsets.json` with its pattern or anchor offset
  and its expected RVA, add a `Check` in `resolve.cpp`, and add an offline test that
  proves the bytes at the site are what the patch expects.
- **Add a fix**: gate it behind a new ini key (default = PS3 behaviour), read the
  key in `config.cpp`, document it in the default-ini text, log one line when it
  installs, and verify the bytes before writing them.
- **Never delete diagnostics**: disable them with their key. They are the record
  of how each root cause was found, and they are how the next one will be.
