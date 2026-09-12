#pragma once
namespace config {
struct Settings {
    int Enabled          = 1;
    int ShowResultScreen = 0;   // no local result screen is correct for these modes;
                                // the original was an ONLINE leaderboard (see config.cpp)
    int ResultScreenId   = 225; // 225 = CActionTougijyoAllStarResult, what our sibling macro opens
    int ResultVariant    = 0;   // 0..3 - which result page of the layout to show
    int ResultHoldFrames = 180; // frames the screen holds before playing its "out" anim
    int TrackBestScore   = 1;   // CActionColosseumExtra+0x208, confirmed 3/4/6
    int DiagDumpFields   = 0;   // dump candidate offsets to the log
    int ResultTimeoutSec = 0;   // seconds; 0 = wait indefinitely (the default now)
                                // Was 120 while the panel had no dismiss handling
                                // and could otherwise hold the game forever. It
                                // has a Close prompt and a button now, so the
                                // engine's own shape - wait for the player, with
                                // no clock - is the correct behaviour.
    int DiagScreenState  = 0;   // log the result screen's own state each second
    int CallTransitionSetup = 0; // stock-result path only (ShowResultScreen=1); the
                                 // validated PS3-parity ini runs with it off
    int ForceFadeIn      = 0;   // after opening, fade the screen back IN (test aid)
    int UnlockAllModes   = 1;   // show all of Bob's rows, not just Battle King
    int DiagStartup      = 0;       // log the ~3s window after the patches install.
                                    // A crash there once stopped the log dead at
                                    // "patches installed", which told us nothing;
                                    // this makes the next occurrence say WHERE.
    int DiagMissionWatch = 0;   // log the running macro + step; names any hang
    int RecapFreeze      = 0;   // hold the recap on screen and open a command channel
                                // Debug scaffolding, off. The console channel
                                // itself still works while the panel is up - the
                                // freeze only ever bought unlimited time to poke.
    int DismissWatchdog  = 0;   // frames before the panel dismisses ITSELF; 0 = never.
                                // The mechanism is kept (kDismissWatchdog documents
                                // the old ~60s value) because it covers two things a
                                // pad read cannot guarantee: that no engine site
                                // cleared the pad edges earlier in the same frame,
                                // and that the pad manager exists at all. If confirm
                                // is ever missed with this at 0, the panel holds
                                // forever and the player is stuck - set it to 3600
                                // to get the escape hatch back.
    int NetRankingPanel  = 1;   // the NETWORK RANKING panel after Battle King /
                                // Fastest Killer. Was 0 while the panel still went
                                // through the minigame widget (which crashed); the
                                // panel has built its own pages since and this is
                                // the value every mode was validated with. A fresh
                                // install writes the ini FROM these defaults, so a
                                // stale 0 here silently switched the panel off.
    int ResetExtrasFlags = 1;   // replay Bob's post-match flag reset so a completed
                                // mode can be started again (see ResetExtrasStartFlags)
    int PanelHideTimeLabel = 1;     // hide page-0 pane 0x22 ("Time") - Battle King is a score mode
    int PanelClosePrompt   = 1;     // raise the X/O Close box (page 16)
    int PanelNumbers     = 1;   // draw Latest/Best via the page-12 pane
    int PanelRollFrames  = 30;      // count-up length; PS3 divides by 30.0
    int PanelBannerFrames= 120;    // full banner length before it parks
    int PanelRecordBanner= 1;       // show page 9's NEW RECORD art on a new best
    int PanelPulseLatest = 1;   // PS3 pulses the LATEST value white<->cyan
    int PanelNumberCellW = 0;       // ctx+0xD90 for the ranking face; 0 = mirror the
                                    // glyph height, which is what a square atlas cell
                                    // needs. Only applied when bank 1 is in use.
    int FixTagHelpPanel  = 1;       // the survival-tag Controls panel draws 1.5x
                                    // too large because it sizes from the output
                                    // resolution instead of the 1280x720 2D canvas
    int FixEndlessHandoff= 1;       // Endless Survival Tag hands off to the DELETED
                                    // mission 143 and skips step 0x2E's teardown ->
                                    // black screen. One byte at +0xB561AF routes it
                                    // down the same chain Survival Tag SP uses.
    int FixChaseDrainRate= 1;       // chase stamina drains 2x too fast: PS3 ticked its
                                    // drain at 30Hz, PC runs the same flat per-frame
                                    // subtraction at 60Hz, and the tuning data is
                                    // byte-identical between the two versions.
                                    // NOTE this affects EVERY chase in the game, story
                                    // chases included - set 0 to leave vanilla pacing.
    int ChaseReplayIntro = 0;       // OFF: DISPROVEN 2026-09-09. Clears CActionChase
                                    // +0x782 bit 0x20 when Bob arms 199:10. The bit
                                    // was already clear on both runs ("intro latch
                                    // already clear" in the log): +0x299B70 starts the
                                    // chase GIMMICKS, not the title card. Kept for the
                                    // record; it does nothing useful. Do not delete.
    int ChaseResetCaptionSeen = 1;  // Speed King intro on every run (PS3 parity).
                                    // Clears 332:10 once the card has set it AND
                                    // 199:10 has dropped (ObserveCaptionSeen, per
                                    // frame - earlier than that the palace replays
                                    // the card), and also with
                                    // the mode-end reset. caption.bin entry 130 ("SPEED
                                    // KING CHAMPIONSHIP" card) requires 332:10==0
                                    // and sets it when the card ends; only Bob's
                                    // retail reset block clears it and PC never
                                    // runs that block. 0 = vanilla PC (card once
                                    // per process).
    int DiagCaptionTrace = 0;       // Speed King intro: the title card is
                                    // CActionStartCaption [g_MainMgr+0x3E8]. Escape
                                    // step 4 (vtable slot 22) SETS it from the current
                                    // sub-mission and slot 107 (the step 0xE entry)
                                    // SHOWS it + plays the jingle. Hooks both slots
                                    // and logs the caption's fields and every gate
                                    // before/after each, plus a per-frame sample when
                                    // anything on the object changes. Bounded.
    int DiagTalkMatch = 0;          // Bob's post-mode line ("What, done already?" /
                                    // "Well? Did you enjoy yourself?") never plays on
                                    // PC. Detours the stage-pac talk matcher
                                    // (+0xBA7AA0) and logs, for blocks whose
                                    // conditions mention the extras flags, the
                                    // runtime entry order, every condition with its
                                    // live FlagGet result, and the slots chosen.
                                    // Logging only; bounded. 0 = no detour.
    int PostModeBobFlags = 1;       // Bob's post-mode line. His pac gates it on
                                    // 332:29 (EXTRA_end) and 332:28 (EXTRA_abort),
                                    // which PS3's mode code sets and PC's never
                                    // does (measured: both read 0 at every
                                    // mode-end). 1 = the mode-end handler sets
                                    // 332:29 after every run and 332:28 too when
                                    // the run produced no result (quit from the
                                    // menu). 0 = leave the flags alone.
    int AmebaFloor = 1;             // Ameba collaboration decal on the coliseum floor
                                    // in Battle King / Fastest Killer (PS3 parity).
                                    // The asset ships on PC; scene object SO033 is
                                    // gated on STID_ST_TOUGI_SCN033 (521:12), and PC's
                                    // coliseum scene-flag function (+0x3927E0) clears
                                    // that flag unconditionally in its tail where PS3
                                    // keeps it. 1 = post-detour that function and, when
                                    // a ranking coliseum mode is armed (199:2..9),
                                    // raise SCN033 / lower SCN003. 0 = no detour.
    int EscapeReplayIntro = 0;      // OFF: UNVERIFIED. Zeroes macro+0x130 once per
                                    // ranked run before step 0xE. Its first version
                                    // read the wrong mission-manager slot and never
                                    // touched the object the intro steps run on, so
                                    // the polarity (does 0 mean "play" or "skip"?) is
                                    // still unproven. DiagEscapeTrace now reads the
                                    // right slot; turn this on only after a trace
                                    // shows +0x130 differing between run 1 and run 2.
    int DiagEscapeTrace  = 0;       // log the Speed King macro's step / handoff fields
                                    // every time they change, for the first ~15s of
                                    // each run. The title card + countdown play on
                                    // the first run of a process and never again
                                    // (a restart restores them); this shows the
                                    // exact step sequence that differs.
    int DiagDrainClock   = 0;       // measure the LOGIC tick rate the drain gate is
                                    // computed from (g_MainMgr+0x1C) and log it when
                                    // it changes. N-of-10 is a halving, correct only
                                    // at 60Hz; this says whether 60Hz is actually
                                    // what the logic runs at. Reports, never retunes.
    int ChaseDrainFramesPer10 = 5;  // how many of every 10 logic frames the drain
                                    // fires on. 5 = PS3 parity, 10 = stock PC (no
                                    // change), 0 = drain DISABLED.
                                    // 0 is the diagnostic: if the gauge still falls
                                    // with the drain off, sub_5A4810 is not the drain
                                    // and nothing here should be trusted. That test
                                    // costs one chase and is what the previous,
                                    // wrong fix could never have passed.
    int FixSteamAchievementCrash = 1;
                                    // Works around a STOCK-GAME startup crash that
                                    // this mod does not cause: the Steam achievement
                                    // callback formats each name/description with
                                    // snprintf("%s"), and the compiler inlined a
                                    // page-unsafe SSE strlen (unaligned 16-byte
                                    // loads, no alignment prologue). Steam returns
                                    // the string at page offset 0xFE2, the second
                                    // load crosses into the next page, and the game
                                    // dies ~5s after launch even though the string
                                    // is correctly terminated. Reproduced 6/6 with
                                    // EVERY mod removed, and once on 2026-08-23 two
                                    // minutes before this project existed.
                                    // 0 = leave the stock behaviour (and the crash).
    int DiagForceEndlessRecord = 0; // TEST HARNESS, off by default. 1 = every Endless
                                    // Survival Tag run shows score 1 and raises the
                                    // NEW RECORD banner, whatever actually happened -
                                    // get caught at once and the recap still fires.
                                    // Nothing is written to the records file. For
                                    // checking the banner sound without a full run.
    int EndlessSuppressResult = 1;  // Endless Survival Tag only: skip the engine's
                                    // own survival result screen so the NETWORK
                                    // RANKING panel is the only thing shown, as on
                                    // PS3. Gated on the tag manager's mode field
                                    // being 1 (Endless), so plain Survival Tag SP
                                    // keeps its result screen exactly as before.
                                    // 0 = engine screen first, then the panel -
                                    // useful to cross-check the score the panel
                                    // reports against the engine's own display.
    int EndlessTagPanel  = 1;       // recap after Endless Survival Tag. Gated on
                                    // flag 199:1, so plain "Survival Tag SP"
                                    // (332:6) keeps its own screen untouched.
    int ChaseWinCode     = 0;       // DEPRECATED and ignored. It selected between
                                    // "codes" on g_MainMgr+0x3D8, which turned out
                                    // to be CActionContinue, not the chase - those
                                    // offsets hold floats there and the read never
                                    // once succeeded. Kept only so existing inis
                                    // still parse. See ChaseRequireOutcome.
    int ChaseRankPanel   = 1;       // recap after Extra Chase / Speed King. Also
                                    // gated on flag 199:10, because mission 301 is
                                    // shared with seven STORY chases.
    int PanelSounds      = 1;       // the recap's four sounds, restored from the PS3
                                    // ranking code: roll-up loop, settle, NEW RECORD,
                                    // confirm. 0 = silent, as the panel was before.
    int PanelSeRoll      = 0x10023; // keys are (bank << 16) | cue, shared with PS3.
    int PanelSeSettle    = 0x10024; // Overridable so a wrong cue can be swapped
    int PanelSeNewRecord = 0x1000C; // without a rebuild (decimal or 0x-hex in the
    int PanelSeDecide    = 0x10001; // ini both parse).
    int PanelRankingFont = 1;       // load the real ranking numeral face into bank 1
                                    // and use it for the digits once it is resident.
                                    // 0 = leave the digits in the panel's label face.
    int PanelNumberFont  = 0;       // ctx+0xDAC - glyph BANK. PS3 uses 1, but MEASURED
                                    // LIVE: banks 1 and 2 have NO font resources
                                    // registered - the global pointer array at RVA
                                    // 0x1990EC0 is all zeros for both, so every glyph
                                    // resolves to -1 and nothing draws at all. Bank 0
                                    // is the only populated bank on PC. Setting 1 here
                                    // makes the digits VANISH; it is kept tunable only
                                    // so the probe can be repeated.
    int PanelNumberMetric= 4;       // ctx+0xDF0 - PS3 uses 4 = condensed metric
    int PanelNumberWidth = 20;      // ctx+0xDA0 - PS3 writes 0x14 for the records
    int PanelPulseDelayMs= 550;     // white for this long after the numbers appear
    int PanelPulseHoldMs = 500;     // then cyan for this long, then white for good
    int PanelTextPrio    = 0x0800;  // ctx+0xD7C - glyph sort priority, below the panel
    int DiagTextBind     = 0;       // log what TextBind established, per slot
    int PanelSortBias    = 0x1000;  // elem+0x4C; must land below the 0x3000 black quad
    int PanelDirectDraw  = 0;   // 0 in the validated ini. Read and logged only; the
                                // engine's own element walk draws the panel.
    int DiagMissionTrace = 0;   // trace the mission manager after a run
    int DiagFlagsOnly    = 0;   // 1 = only LOG which start flags are stuck, clear nothing
                                // crashes. See netrank.cpp before re-enabling.
};
const Settings& Get();   // reads HouseOfExtras.ini once, writes defaults if absent
}
