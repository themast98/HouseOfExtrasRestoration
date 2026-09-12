// Replacement for CMissionMacroColosseumExtra's deleted step 0x2C handler.
//
// Sega gutted this to `ret 0` when they removed src/ranking, so the macro parked
// at step 0x2C with no next step queued -> infinite black screen.
//
// A step handler is invoked EVERY FRAME while it is the current step - that is
// precisely why the `ret 0` stub span forever. We use that to run the whole
// recap lifecycle here:
//
//   Fresh     build the real panel on pjs_net_ranking (netrank::Open), or
//             fall back to opening a stock result screen if that is off
//   Opening   stock path only: wait for the screen slot to fill   (bounded)
//   Loading   stock path, caption ids only: wait for the layout, then call the
//             engine's own variant setter
//   Showing   drive our panel, or wait for a stock action to close (bounded)
//   done      queue step 0x2E (cleanup / back to Naomi's Palace)
//
// THE REAL PANEL. PS3 footage with no PSN connection shows the end screen as
// NETWORK RANKING / -Battle King Ranking (Kiryu)- / Score / Latest Record /
// Best Record - a small LOCAL panel, not a leaderboard. Its layout
// (pjs_net_ranking) and all of its artwork still ship on PC; only the board
// name and the two numbers were lost with src/ranking. See netrank.cpp.
//
// WHICH SCREEN. OpenScreen maps an id through a range table to a factory, and
// the ids are NOT interchangeable:
//     220 -> CActionSurvivalCaption          a caption banner
//     222 -> CActionSurvivalBattleResult     the survival-battle recap
//     223 -> CActionSurvivalOnigokkoResult   the tag-mode recap
//     225 -> CActionTougijyoAllStarResult    the Coliseum tournament result
// This mod drove 220 for a long time and could not understand why an object
// that loaded, ticked at 60fps, animated to completion and had an open draw
// gate still showed nothing: it was the caption, and captions are suppressed
// outside the states that show them. Its four "variants" are the
// svbtl_clear / congra / mission / boss banner art, not recap pages.
//
// A result action needs none of the caption dance - it loads, displays, waits
// for the player and closes itself. And the caption setter must NEVER be
// pointed at one: it writes a variant to +0x1B8, where a result action keeps an
// allocated pointer, so the call would corrupt it.
//
// Live screens are kept in an array indexed by SCREEN ID:
//     slot(id) = mainMgr + 0x1E8 + id*8
//
// THE HANDOVER TO 0x2D. This used to hold the panel here at 0x2C on a 120s
// timeout. That was wrong twice over: the mode's own end-of-run moment is not
// where anything 2D reaches the screen, and the original has no timer at all -
// PS3's step handler configures the panel and its NEXT step polls the screen
// slot, advancing only when the player presses X/O. PC's macro has the same
// two-step shape (the engine's 0x2D, sub_B60910, is that same slot poll), so
// 0x2C now does its work once and hands over. Our 0x2D drives the panel and
// waits on the player; the only bound left is a 60s anti-softlock watchdog.

#include "patch.h"
#include "game.h"
#include "config.h"
#include "records.h"
#include "probe.h"
#include "input.h"
#include "console.h"
#include "netrank.h"
#include "resolve.h"   // the chase hook resolves AdventureEscape's vtable
#include "offsets.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>

namespace {

// Matches enemy_dispose/ene_st_tougi_8103.bin, the set Battle King uses.
constexpr int kModeBattleKing = 8103;

// One record slot PER BOARD. All eight used to share kModeBattleKing, which is
// why a Fastest Killer panel displayed 11 - a Battle King kill count - as its
// "Best Record". 8103 is retained as the fallback and happens to coincide with
// board index 3, so an existing file keeps its meaning.
constexpr int kModeExtrasBase = 8100;

// Read whichever value THIS board is scored by, with the record key to match.
// A time board is read from the action's CActionTimer and converted to the
// composite value PS3 stores; a score board keeps the kill counter. Reading the
// kill counter on a time board can only ever yield a stale zero, because the
// engine never increments it outside the score branch - which is exactly why
// Fastest Killer showed "Latest Record 0".
struct RunResult { int key; int value; bool timeMode; };

// The best as it stood BEFORE this run, plus whether this run beat it.
//
// The panel must show the OLD record while the new one is announced - in the PS3
// capture the Best row reads 0:02'05"81 while Latest rolls up to 0:02'04"55 and
// the NEW RECORD banner appears. Displaying records::GetBest() after the store
// would put the same figure in both rows and make the banner meaningless.
int  s_prevBest  = 0;
bool s_wasRecord = false;

RunResult ReadRun() {
    const bool timeMode = game::IsTimeMode();
    const int  idx      = game::ReadModeIndex();
    const int  key      = (idx >= 0 && idx <= 7) ? (kModeExtrasBase + idx)
                                                 : kModeBattleKing;
    int value = 0;
    if (timeMode) {
        const int frames = game::ReadRunFrames();
        if (frames > 0) value = game::RunFramesToPS3Value(frames);
    } else {
        const int kills = game::ReadKillCount();
        if (kills > 0) value = kills;
    }
    netrank::SetTimeMode(timeMode);
    return { key, value, timeMode };
}

// ~2s at 60fps. The slot is written synchronously inside OpenScreen, so this
// only ever catches genuine failure.
constexpr int kOpenTimeout = 120;

// ~5s at 60fps for the layout to stream in.
constexpr int kLoadTimeout = 300;

// CActionColosseumExtra is allocated with size 0x218.
constexpr uintptr_t kActionSize = 0x218;

// A gap between calls means we re-entered the step. Keying off the `self`
// pointer instead would resume stale state whenever the macro left the step
// without Finish() running (e.g. quit to title mid-recap).
constexpr uint64_t kFreshGapMs = 500;

enum class Stage { Fresh, Opening, Loading, Showing };

Stage    s_stage    = Stage::Fresh;
int      s_frames   = 0;
uint64_t s_lastTick = 0;
void*    s_screen   = nullptr;
bool     s_panel    = false;   // our own pjs_net_ranking panel is up

// What each screen id actually constructs. OpenScreen maps an id through a
// range table (lo at RVA 0x12363E0, hi at 0x1236850) to an index, then jumps
// to a factory in the table at RVA 0x16E4910:
//
//   217 -> CActionSurvivalBattleManager
//   220 -> CActionSurvivalCaption          a CAPTION BANNER, not a recap
//   222 -> CActionSurvivalBattleResult     the survival-battle recap
//   223 -> CActionSurvivalOnigokkoResult   the tag-mode recap
//   225 -> CActionTougijyoAllStarResult    what the Coliseum reference opens
//
// This mod spent a long time driving 220 and wondering why a perfectly healthy
// object showed nothing: it is the caption, whose four "variants" are the
// svbtl_clear / congra / mission / boss banner art.
constexpr int kCaptionScreenId = 220;

// The caption-only setter. CActionSurvivalCaption keeps a variant at +0x1B8;
// CActionSurvivalBattleResult keeps an ALLOCATED POINTER there, so calling the
// setter on a result action would overwrite that pointer and crash. Hence this
// is gated on the exact id rather than on "is a screen open".
bool UsesCaptionSetter(int id) { return id == kCaptionScreenId; }

// Screen object sizes, from each factory's allocation site. Every diagnostic
// read is bounded by these so the probe cannot run off the end of the object.
// An unknown id returns 0, which disables field probing entirely - the field
// MEANINGS are per-class, so probing an unrecognised class would be nonsense
// even where it is in-bounds.
size_t ScreenSize(int id) {
    if (id == 220) return 0x1C8;   // sub_333260: mov ecx, 0x1C8
    if (id == 222) return 0x1D8;   // sub_3331D0: mov ecx, 0x1D8
    if (id == 225) return 0x220;   // sub_333A40: mov ecx, 0x220
    return 0;
}

int ScreenField(int id, uintptr_t o) {
    const size_t size = ScreenSize(id);
    if (!s_screen || o + 4 > size) return -1;
    return *(int*)((unsigned char*)s_screen + o);
}

void LogScreenState(const char* when, int id) {
    if (!s_screen) return;
    if (!ScreenSize(id)) { hoe::Log("  screen %s: unknown id %d, not probing", when, id); return; }
    hoe::Log("  screen %s: flags=0x%08X loaded[+1A8]=%d variant[+1B8]=%d drawgate[+1C0]=%d",
             when, (unsigned)ScreenField(id, 8),
             ScreenField(id, off::C_SCREEN_LOADED_FIELD),
             ScreenField(id, off::C_SCREEN_VARIANT_FIELD),
             ScreenField(id, off::C_SCREEN_DRAWGATE_FIELD));
}

// Walk screen -> layout -> page[variant] -> element and report the two flags
// that decide whether anything is actually rasterised:
//   elem[+0x2C] bit 0 - armed by the layout's "in" animation; the element
//                       updater returns immediately unless it is set
//   elem[+0xBC]       - non-zero suppresses the draw
// The gate at screen+0x1C0 being open only means Draw is entered; these decide
// whether it puts pixels anywhere.
void LogElementState(const char* when, int variant) {
    if (!s_screen) return;
    auto scr = (unsigned char*)s_screen;
    auto layout = *(unsigned char**)(scr + off::C_SCREEN_LAYOUT_FIELD);
    if (!layout) { hoe::Log("  elem %s: layout is NULL", when); return; }
    auto pages = *(unsigned char**)(layout + off::C_LAYOUT_PAGE_ARRAY);
    if (!pages) { hoe::Log("  elem %s: page array is NULL", when); return; }
    auto page = pages + (uintptr_t)variant * off::C_LAYOUT_PAGE_STRIDE;
    auto elem = *(unsigned char**)(page + off::C_PAGE_ELEM_FIELD);
    if (!elem) { hoe::Log("  elem %s: page %d element is NULL", when, variant); return; }
    const unsigned flags = *(unsigned*)(elem + off::C_ELEM_FLAGS_FIELD);
    hoe::Log("  elem %s: layout=%p page%d=%p elem=%p flags[+2C]=0x%08X armed=%d suppress[+BC]=%d",
             when, layout, variant, page, elem, flags, (flags & 1) ? 1 : 0,
             *(int*)(elem + off::C_ELEM_SUPPRESS_FIELD));
    game::LogLayoutResources(when, layout);
    static bool s_dumped = false;
    // Once per session is enough - it is a comparison, not a time series.
    if (!s_dumped) { s_dumped = true; game::LogAllLayoutSlots(when); }
}

// A result action that dereferences another action's object without checking
// it. Opening one of these while its prerequisite is absent faults inside the
// game's own constructor, so we check first.
//
//   222 CActionSurvivalBattleResult   -> slot 217 CActionSurvivalBattleManager
//                                        loaded at sub_3A3A55, used unchecked
//   223 CActionSurvivalOnigokkoResult -> slot 212, which it DOES null-check;
//                                        listed anyway so the rule is uniform
int RequiredScreenId(int id) {
    if (id == 222) return 217;
    if (id == 223) return 212;
    return -1;
}

uint64_t ScreenSlot(void* mgr, int screenId);

// The screens that decide which recap is even possible in this mode.
void LogRelevantScreens(void* mgr) {
    static const struct { int id; const char* what; } kWatch[] = {
        {212, "onigokko prerequisite"},
        {217, "CActionSurvivalBattleManager (222 needs this)"},
        {220, "CActionSurvivalCaption"},
        {222, "CActionSurvivalBattleResult"},
        {223, "CActionSurvivalOnigokkoResult"},
        {225, "CActionTougijyoAllStarResult"},
    };
    for (const auto& w : kWatch) {
        const uint64_t p = ScreenSlot(mgr, w.id);
        hoe::Log("  screens: %3d %-46s %s", w.id, w.what, p ? "OPEN" : "-");
    }
}

uint64_t ScreenSlot(void* mgr, int screenId) {
    // screenId is range-checked in config::Load(), so this stays in the array.
    const uintptr_t off = off::C_SCREEN_SLOT_BASE + (uintptr_t)screenId * 8;
    return *(uint64_t*)((unsigned char*)mgr + off);
}

// --- the mode teardown the deleted step 0x2D was supposed to call ----------
//
// Recovered from the PS3 EBOOT, where this class survives intact. PS3's
// CMissionMacroColosseumExtra::Step2D (0x436540) ends:
//
//     if (kind == 0) { SetNextStep(this, 0x2E); return; }
//     this->vfunc11(kind);        // PS3 slot 11 == PC slot 10 == +0xB58EE0
//     SetNextStep(this, 0);       // this class terminates on step 0, not 0x2E
//
// +0xB58EE0 is PRESENT AND INTACT in the PC binary. Sega did not delete it - it
// merely became unreachable when its only caller, step 0x2D, was gutted. Never
// calling it is why finishing any House of Extras mode left the game wrecked
// until a restart: the completion branch releases the mode object
// (+0xB4CC70(mode, 0)) and writes the mode's "finished" flag, which is what
// Bob's menu reads to decide whether the mode can be started again.
//
// SAFETY. The completion branch chases a SELF-RELATIVE offset:
//     r8  = flagMgr->[0x990] + 0x2C
//     rcx = *(int*)r8 ? r8 + *(int*)r8 : 0
//     edx = *(u16*)(rcx + 0xD08)      <-- null-derefs when that int is zero
// so that exact case is checked below and refused rather than crashed. Every
// other dereference the function makes is guarded here too, and the vtable slot
// is followed through its incremental-link thunk and compared against the
// address actually reverse-engineered before anything is called.
bool CallModeTeardown(void* self, int kind) {
    // kind == 0 is not a failure: it is the engine's own "no result" path, and
    // PS3 routes it to 0x2E WITHOUT the teardown. Mirror that exactly.
    if (!self || kind == 0) return false;

    void** vft = *(void***)self;
    if (!vft) { hoe::Log("teardown: macro has no vtable - skipped"); return false; }

    auto slot = (unsigned char*)vft[off::SLOT_MODE_TEARDOWN];
    if (!slot) {
        hoe::Log("teardown: vtable slot %d is null - skipped", off::SLOT_MODE_TEARDOWN);
        return false;
    }

    // Vtable entries are MSVC incremental-link E9 rel32 thunks; follow one hop.
    unsigned char* target = slot;
    if (*slot == 0xE9) target = slot + 5 + *(int32_t*)(slot + 1);
    const uintptr_t rva = (uintptr_t)target - hoe::Base();
    if (rva != off::EXPECT_MODE_TEARDOWN) {
        hoe::Log("teardown: slot %d resolves to +0x%llX, expected +0x%llX - "
                 "refusing to call", off::SLOT_MODE_TEARDOWN,
                 (unsigned long long)rva,
                 (unsigned long long)off::EXPECT_MODE_TEARDOWN);
        return false;
    }

    if (!*(void**)((unsigned char*)self + off::C_MISSION_FIELD)) {
        hoe::Log("teardown: no mode object at macro+0x%llX - skipped",
                 (unsigned long long)off::C_MISSION_FIELD);
        return false;
    }

    void* flagMgr = *game::pFlagMgr;
    if (!flagMgr) { hoe::Log("teardown: no flag manager - skipped"); return false; }

    auto table = *(unsigned char**)((unsigned char*)flagMgr + off::C_FLAGMGR_TABLE_FIELD);
    if (!table) { hoe::Log("teardown: flag table null - skipped"); return false; }

    auto desc = table + off::C_FLAGMGR_DESC_REL;
    if (*(int32_t*)desc == 0) {
        hoe::Log("teardown: mode descriptor offset at flagMgr->[0x%llX]+0x%llX is "
                 "zero - the engine function would null-deref, refusing to call",
                 (unsigned long long)off::C_FLAGMGR_TABLE_FIELD,
                 (unsigned long long)off::C_FLAGMGR_DESC_REL);
        return false;
    }

    ((game::ModeTeardownFn)slot)(self, kind);
    hoe::Log("teardown: +0x%llX(macro, kind=%d) ran - mode released, completion "
             "flag written", (unsigned long long)off::EXPECT_MODE_TEARDOWN, kind);
    return true;
}

// The single exit for the whole recap, from BOTH step handlers. Every route
// through here is the end of a mode the player actually finished, including the
// error routes - a panel that failed to build is no reason to leave the game
// broken - so the teardown runs on all of them.
void Finish(void* self, const char* why) {
    if (s_panel) { netrank::Close(); s_panel = false; }

    const int kind = *(int*)((unsigned char*)self + off::C_MACRO_RESULT_KIND);
    if (kind != 0) game::NoteRunResult("coliseum");
    const bool tore = CallModeTeardown(self, kind);

    // ALWAYS 0x2E - never step 0, even though PS3's 0x2D ends SetNextStep(this, 0).
    //
    // MEASURED LIVE, do not "correct" this back to match PS3. PC's step 0 handler
    // (vtable slot 18, +0xB527E0) is a `ret 0` stub, so routing there skips step
    // 0x2E (+0xB595E0) and its two mode-object cleanup calls, +0xB4BD80 and
    // +0xB4BDE0. With step 0 the macro does die - missionMgr->macro reads null -
    // but the Adventure macro that restores the palace NEVER STARTS and the
    // screen stays black indefinitely. With 0x2E the palace comes back, as it
    // did in every build before this one.
    //
    // 0x2E ends in SetNextStep(0) itself, so this still reaches the same terminal
    // step - just through the engine's own cleanup rather than bypassing it. The
    // teardown above is what the old builds were missing; the destination never
    // was. C_STEP_TERMINAL is deliberately left unused.
    game::SetNextStep(self, (int)off::C_STEP_2E);
    hoe::Log("recap: %s -> %s step 0x2E (exit, kind=%d)", why,
             tore ? "teardown ran," : "no teardown,", kind);

    // The run is over. Arm the flag reset - it fires from the heartbeat once the
    // macro is actually destroyed. Without it the mode's own "start" bit stays 1
    // and Bob's row silently does nothing forever, because starting a mode is
    // edge-triggered on that flag rather than being a command.
    game::ArmExtrasReset(self);

    s_stage = Stage::Fresh;
    s_frames = 0;
    s_lastTick = 0;
    s_screen = nullptr;
    // Clear the carried record state, or the NEXT run's panel would show this
    // run's previous best and could re-raise the banner without earning it.
    s_prevBest = 0;
    s_wasRecord = false;
    netrank::SetNewRecord(false);
}

// Optional field dump used to hunt the real score offset. Off by default.
void DumpCandidates() {
    void* mgr = *game::pMainMgr;
    if (!mgr) { hoe::Log("  diag: no mainMgr"); return; }
    auto action = *(unsigned char**)((unsigned char*)mgr + off::C_ACTION_OFFSET);
    if (!action) { hoe::Log("  diag: no action object"); return; }

    // mainMgr+0xBA0 is a polymorphic "current action" slot, so confirm the
    // concrete type before reading fields off it.
    const void* vft = *(void**)action;
    if (!game::IsColosseumExtra(vft)) {
        hoe::Log("  diag: [mainMgr+0xBA0] is not CActionColosseumExtra, skipping");
        return;
    }
    char line[256];
    for (uintptr_t o = 0x180; o + 16 <= kActionSize; o += 16) {
        snprintf(line, sizeof(line), "  diag: +0x%03llX  %11d %11d %11d %11d",
                 (unsigned long long)o,
                 *(int*)(action + o), *(int*)(action + o + 4),
                 *(int*)(action + o + 8), *(int*)(action + o + 12));
        hoe::Log("%s", line);
    }
}


// --- the step-0x2D handover -------------------------------------------------
//
// SetNextStep (sub_B52B20) is literally `mov [rcx+0x12c], edx ; ret`, and the
// driver latches +0x12C into +0x128 on a later pass and rewrites +0x12C to -1
// (sub_B52AF0). So the advance is one store, it is not re-entrant, and 0x2D
// will not run inside this frame. Validated live before use: +0x128 read 0x2C
// and +0x12C read -1 on every sampled frame.
void AdvanceStep(void* self, int step) {
    *(int*)((unsigned char*)self + off::C_MACRO_STEP_PENDING) = step;
}

// Dismissal state for the 0x2D handler. File-static rather than fields on the
// macro object: the object layout past +0x12C is unmapped, and writing into it
// is the failure class that has already cost this project three crashes.
bool s_armed    = false;   // seen a frame with confirm NOT held
int  s_holdFrames = 0;

// ~60s. NOT the dismissal - that is the button. This only covers the two things
// a read cannot guarantee: that no engine site cleared the pad edges earlier in
// the same frame, and that the pad manager exists at all.
constexpr int kDismissWatchdog = 3600;

}  // namespace

// Step 0x2D. The engine's own handler here polls screen slot(225) with no
// timeout and advances to 0x2E when the player closes that screen - the exact
// shape PS3 used (its waiter polls screen slot 233). We keep the shape and
// change only the condition, because our panel is a bare layout rather than a
// screen and so has no slot to clear.
//
// This is where the panel now LIVES: 0x2C runs once and hands over, so the
// build loop, the show and the per-frame upkeep all happen here.
extern "C" __attribute__((ms_abi)) void HoE_Step2D(void* self) {
    // Both of these are still the end of a completed mode, so they exit through
    // Finish() and get the teardown like every other route.
    if (!s_panel) { Finish(self, "step 0x2D reached with no panel"); return; }

    if (netrank::Failed()) {
        hoe::Log("step 0x2D: panel could not be built");
        Finish(self, "panel build failed");
        return;
    }

    // The layout takes a few frames to stream in; Show() is a no-op until it is
    // ready. Draw() maintains the texture binding and the dynamic strings.
    const bool ready = netrank::Ready();
    if (ready) {
        const RunResult run = ReadRun();
        netrank::Show(run.value, s_prevBest ? s_prevBest : records::GetBest(run.key));
    }
    netrank::Draw();
    // Drive the element draw ourselves rather than relying on the sorted global
    // pass - see netrank::DrawDirect. Logged once so a null result is visible in
    // the log rather than being indistinguishable from "never ran".
    if (config::Get().PanelDirectDraw) {
        const int n = netrank::DrawDirect();
        static int s_lastN = -1;
        if (n != s_lastN) {
            s_lastN = n;
            hoe::Log("netrank: DrawDirect drew %d element(s) this frame", n);
        }
    }
    if (!ready) return;          // still building - do not count hold frames yet

    ++s_holdFrames;
    if (s_holdFrames < 2) return;   // one dead frame so the fight-ending press
                                    // cannot leak straight into the panel

    const input::Pad pad = input::Read();

    // Arm only after a frame where confirm is NOT held. Without this the press
    // that ended the fight would dismiss the panel before it was seen - which
    // is exactly what the old 120s timeout was papering over.
    if (pad.ok && !s_armed) {
        if (!input::ConfirmHeld(pad)) s_armed = true;
        return;
    }

    // RecapFreeze used to act on the Showing stage, which the panel no longer
    // passes through. Without this the console would be unreachable while the
    // panel is up, and the console is how the remaining unknowns (which page
    // and bind mode each dynamic string wants) get measured.
    if (config::Get().RecapFreeze && !console::ResumeRequested()) {
        probe::PadEdges();
        console::Pump(true);
        return;
    }

    // The watchdog is DISABLED by default now (DismissWatchdog = 0). The panel
    // has a working Close prompt, so the engine's own shape - hold until the
    // player presses, with no clock - is what should happen. kDismissWatchdog
    // stays as the documented value to restore if confirm is ever missed.
    const int  watchdog = config::Get().DismissWatchdog;
    const bool pressed  = s_armed && input::ConfirmEdge(pad);
    if (pressed && config::Get().PanelSounds) game::PlaySE((unsigned)config::Get().PanelSeDecide);
    const bool expired  = watchdog > 0 && s_holdFrames > watchdog;
    if (!pressed && !expired) return;   // hold, exactly as the engine handler does

    hoe::Log("step 0x2D: panel dismissed after %d frames (%s)",
             s_holdFrames, pressed ? "confirm pressed" : "watchdog");
    Finish(self, "panel dismissed");
}

extern "C" __attribute__((ms_abi)) void HoE_Step2C(void* self) {
    const auto& cfg = config::Get();

    const uint64_t now = GetTickCount64();
    if (s_lastTick == 0 || now - s_lastTick > kFreshGapMs) {
        s_stage = Stage::Fresh;
        s_frames = 0;
    }
    s_lastTick = now;

    void* mgr = *game::pMainMgr;
    if (!mgr) { Finish(self, "no main manager"); return; }

    switch (s_stage) {

    case Stage::Fresh: {
        if (cfg.DiagDumpFields) DumpCandidates();

        const RunResult run = ReadRun();
        const int kills = run.value;
        if (cfg.TrackBestScore && run.value > 0) {
            const int prev = records::GetBest(run.key);
            const bool best = records::SetBest(run.key, run.value, run.timeMode);
            s_prevBest  = prev;
            s_wasRecord = best;
            netrank::SetNewRecord(best);
            hoe::Log("step 0x2C: %s=%d previous best=%d%s (board key %d)",
                     run.timeMode ? "time" : "score", run.value, prev,
                     best ? " (NEW BEST)" : "", run.key);
        } else {
            // Three different things used to share one misleading message. Only
            // a negative return means the field could not be read: ReadKillCount
            // returns -1 when the manager, the action or its type check fails,
            // and the real value otherwise - and that value is legitimately 0
            // for a run that scored nothing. Reporting a true 0 as "unresolved"
            // sent a past session hunting a field that was never broken.
            if (kills < 0) {
                hoe::Log("step 0x2C: score field unresolved (no live "
                         "CActionColosseumExtra), not recorded");
            } else if (!cfg.TrackBestScore) {
                hoe::Log("step 0x2C: score=%d, not recorded (TrackBestScore=0)", kills);
            } else {
                hoe::Log("step 0x2C: score=0 - a real zero, nothing to record");
            }
        }

        // The real end-of-mode panel: rebuild it on pjs_net_ranking rather than
        // borrowing another mode's result screen. Falls through to the old
        // screen-opening path only if this is off or could not be built.
        // Hand the panel to step 0x2D and get out. Holding here is what put
        // the panel on screen during the mode's own fade-and-teardown moment,
        // where nothing 2D was reaching the screen; and the 120s timeout it
        // needed is not how the original behaves - PS3 waits on the player,
        // with no timer, and PC's macro has the same two-step shape.
        if (cfg.NetRankingPanel && netrank::Open()) {
            // This used to replicate +0xB5C320's five calls (TransitionSetup /
            // CommitFlags / OpenScreen(225) / SetNextStep / PostOpenNotify).
            // They are GONE because that is CMissionMacroColosseum's step 0x2C,
            // a DIFFERENT CLASS. Copying the sibling was the modelling error
            // behind two failed fixes; the fade in particular drove the overlay
            // opaque for no reason at all.
            //
            // ColosseumExtra's own 0x2C (PS3 0x4364C8) is:
            //     rm = g_mainMgr->[0x5A0];        if (!rm) return;
            //     if (this->[0x104]) sub_6102B4(rm, 2, 1);
            //     SetNextStep(this, 0x2D);
            // and its 0x2D polls rm->[0x104] before tearing down.
            //
            // NEITHER OF THOSE TWO PIECES EXISTS ON PC - searched exhaustively
            // and confirmed by an independent verifier. Do not go looking again:
            //   * sub_6102B4 is NOT "open the result panel". It is
            //     CScreenEffectFocusBlur::Start(rm, durationFrames, enable) - a
            //     depth-of-field post-process. rm->[0x104] is its "effect
            //     finished" marker, nothing to do with the player or the panel.
            //     No PC function implements it; its 4-flag table {0x40D..0x410}
            //     has no PC code reference at all.
            //   * rm itself is a 0x284-byte action registered at PS3 action id
            //     241 (factory 0x664960, ctor 0x60F8F8). The PC action-id table
            //     diverges after id 230, so PS3 240/241/242 have NO PC id.
            //
            // DANGER: PC mgr + 0x1E8 + 241*8 == mgr+0x970 is a REAL, LIVE slot -
            // CActionPlaygoPopup, RTTI-confirmed. Reading it passes a null check
            // and hands back a valid pointer of the WRONG TYPE. Writing rm's
            // fields through it is silent heap corruption with a delayed crash.
            // Never use 0x970 as the result sub-manager.
            //
            // What we lose by skipping all this is a blur transition. Cosmetic.
            // Our panel substitutes for the engine's, so all that remains here
            // is the advance. The teardown happens at 0x2D, through Finish().
            s_panel      = true;
            s_armed      = false;
            s_holdFrames = 0;
            s_stage      = Stage::Fresh;   // 0x2C is done; nothing re-enters it
            probe::Reset();
            hoe::Log("step 0x2C: panel opened -> step 0x2D (waits on the player)");
            AdvanceStep(self, (int)off::C_STEP_2D);
            return;
        }

        if (!cfg.ShowResultScreen) { Finish(self, "results disabled"); return; }

        // Which screens are live right now. This is what tells us whether a
        // result action's prerequisite is even available in this mode.
        if (cfg.DiagScreenState) LogRelevantScreens(mgr);

        // A result action may dereference another action without checking it.
        // CActionSurvivalBattleResult (222) loads mainMgr+0x8B0 = slot(217) at
        // sub_3A3A55 and passes it straight to sub_3A3230, which does
        // `cmp [rcx+0x1a8], ebx` - an unconditional dereference. The null test
        // just above it guards mainMgr, NOT the pointer it loads. So opening
        // 222 without a CActionSurvivalBattleManager is a hard crash inside
        // Sega's own constructor, and it is our job never to trigger it.
        const int need = RequiredScreenId(cfg.ResultScreenId);
        if (need >= 0 && ScreenSlot(mgr, need) == 0) {
            hoe::Log("step 0x2C: screen %d requires screen %d to be open, and it "
                     "is not - refusing to open it (that would crash inside the "
                     "game's constructor)", cfg.ResultScreenId, need);
            Finish(self, "result screen prerequisite missing");
            return;
        }

        void* mission = *(void**)((unsigned char*)self + off::C_MISSION_FIELD);
        if (cfg.CallTransitionSetup) {
            game::TransitionSetup(mission, 1, *game::pKFloat, 0, 0);
        } else {
            hoe::Log("step 0x2C: TransitionSetup skipped (CallTransitionSetup=0)");
        }
        game::CommitFlags(*game::pFlagMgr);

        void* arg3 = *(void**)((unsigned char*)mgr + off::C_OPENSCREEN_ARG3_FIELD);
        // Logged BEFORE the call. OpenScreen runs a constructor that can fault,
        // and when it did the log simply stopped with no indication of where -
        // this line makes the next crash self-identifying.
        hoe::Log("step 0x2C: opening screen %d...", cfg.ResultScreenId);
        void* scr  = game::OpenScreen(mgr, cfg.ResultScreenId, arg3);

        // The reference handler does `lea edx,[r9+0x1D]` with r9d==1, so the
        // argument is 0x1E - not the 0x1D a naive read of the immediate gives.
        game::PostOpenNotify(mission, 0x1E, 0, 1, 1);

        hoe::Log("step 0x2C: OpenScreen(%d) -> %p, parent=%p", cfg.ResultScreenId, scr, arg3);
        if (!scr) { Finish(self, "OpenScreen returned null"); return; }

        // Test aid, and it was WRONG until measured live. TransitionSetup picks
        // its target alpha as `(mode == 0 || mode == 1) ? 1.0f : 0.0f` - the
        // constant is at RVA 0x1209ABC and really is 1.0 - so mode 0 fades to
        // OPAQUE, not transparent. This call used mode 0 and was therefore
        // driving the overlay to black, the exact opposite of "fade in": reading
        // the live fade state during a frozen recap showed alpha 1.0 with the
        // state byte set to 2 (still fading up).
        //
        // Any mode outside {0,1} targets alpha 0; 2 is used here. Clearing the
        // overlay does reveal 2D UI that was hidden underneath it, so the effect
        // is real - it simply is not what makes the recap panel appear.
        if (cfg.ForceFadeIn) {
            game::TransitionSetup(mission, 2, 1.0f, 0, 1);
            hoe::Log("step 0x2C: ForceFadeIn - fading the overlay OUT (mode 2)");
        }

        s_screen = scr;
        if (cfg.DiagScreenState) LogScreenState("at open", cfg.ResultScreenId);
        s_stage = Stage::Opening;
        s_frames = 0;
        return;
    }

    case Stage::Opening: {
        ++s_frames;
        if (ScreenSlot(mgr, cfg.ResultScreenId) != 0) {
            hoe::Log("step 0x2C: screen registered after %d frames", s_frames);
            s_stage = Stage::Loading;
            s_frames = 0;
            return;
        }
        if (s_frames >= kOpenTimeout) Finish(self, "screen slot never populated");
        return;
    }

    case Stage::Loading: {
        ++s_frames;
        // A result action drives its own lifecycle: it loads, shows, waits for
        // the player and closes itself. There is nothing for us to poke, and
        // its fields do not mean what the caption's do, so go straight to
        // waiting for it to finish.
        if (!UsesCaptionSetter(cfg.ResultScreenId)) {
            hoe::Log("step 0x2C: screen %d is a self-driving result action; "
                     "waiting for it to finish", cfg.ResultScreenId);
            s_stage = Stage::Showing;
            s_frames = 0;
            return;
        }
        // The setter dereferences the layout page array, so wait for the load.
        if (ScreenField(cfg.ResultScreenId, off::C_SCREEN_LOADED_FIELD) > 0) {
            // Freezing holds the banner open by giving it a hold count it will
            // never reach, rather than by fighting the draw gate every frame.
            const int hold = cfg.RecapFreeze ? 100000000 : cfg.ResultHoldFrames;
            game::SetResultVariant(s_screen, cfg.ResultVariant, hold);
            hoe::Log("step 0x2C: layout loaded after %d frames; "
                     "SetResultVariant(variant=%d, hold=%d)%s",
                     s_frames, cfg.ResultVariant, hold,
                     cfg.RecapFreeze ? "  [FROZEN - send commands via HouseOfExtras.cmd]" : "");
            if (cfg.DiagScreenState) {
                LogScreenState("after setter", cfg.ResultScreenId);
                LogElementState("after setter", cfg.ResultVariant);
            }
            s_stage = Stage::Showing;
            s_frames = 0;
            return;
        }
        if (cfg.DiagScreenState && s_frames % 60 == 0) {
            LogScreenState("loading", cfg.ResultScreenId);
        }
        if (s_frames >= kLoadTimeout) {
            LogScreenState("load timeout", cfg.ResultScreenId);
            Finish(self, "layout never finished loading");
        }
        return;
    }

    case Stage::Showing: {
        ++s_frames;
        if (s_panel) {
            if (netrank::Failed()) {
                netrank::Close();
                s_panel = false;
                Finish(self, "net-ranking panel could not be built");
                return;
            }
            if (netrank::Ready()) {
                const RunResult run = ReadRun();
                netrank::Show(run.value, s_prevBest ? s_prevBest : records::GetBest(run.key));
            }
            netrank::Draw();
            const bool frozenPanel = cfg.RecapFreeze && !console::ResumeRequested();
            if (frozenPanel) {
                // Reads only. Every sample is taken from this one place - game
                // thread, same point in the frame - because two earlier
                // conclusions in this project came from comparing samples
                // gathered in different ways.
                probe::PadEdges();
                probe::StepAndPresent(self);
                console::Pump(true);
                return;
            }
            // Bounded like every other wait here: the panel has no dismiss
            // handling yet, so it must not be able to hold the game forever.
            if (cfg.ResultTimeoutSec > 0 &&
                s_frames > cfg.ResultTimeoutSec * 60) {
                netrank::Close();
                s_panel = false;
                Finish(self, "net-ranking panel timed out");
            }
            return;
        }
        const bool frozen = cfg.RecapFreeze && !console::ResumeRequested();
        if (frozen) {
            // Runs on the game thread, which is the only place engine calls
            // that touch the layout system are safe.
            console::Pump(true);
        }
        if (ScreenSlot(mgr, cfg.ResultScreenId) == 0) {
            Finish(self, "screen closed itself");
            return;
        }
        if (frozen) return;   // every exit below is disabled while frozen
        // Screen 220 is a TIMED BANNER, not an interactive panel: it has no
        // confirm-button state (that belongs to screen 225), and it never clears
        // its own slot. It simply shuts its draw gate once ResultHoldFrames have
        // elapsed. Waiting on the slot therefore always ran to the timeout, so
        // treat the gate closing as the natural end of the banner.
        // The draw-gate test is the CAPTION's end condition: screen 220 is a
        // timed banner that never clears its own slot, it just shuts its gate.
        // A result action does clear its slot, which the check above catches.
        if (UsesCaptionSetter(cfg.ResultScreenId) &&
            ScreenField(cfg.ResultScreenId, off::C_SCREEN_DRAWGATE_FIELD) == 0) {
            Finish(self, "banner finished (draw gate closed)");
            return;
        }
        if (cfg.DiagScreenState && s_frames % 120 == 0 && s_frames <= 600) {
            char when[32];
            snprintf(when, sizeof(when), "showing f%d", s_frames);
            LogScreenState(when, cfg.ResultScreenId);
            if (UsesCaptionSetter(cfg.ResultScreenId)) {
                LogElementState(when, cfg.ResultVariant);
            }
        }
        // 0 disables the timeout entirely (wait for the player indefinitely).
        if (cfg.ResultTimeoutSec > 0 &&
            (int64_t)s_frames >= (int64_t)cfg.ResultTimeoutSec * 60) {
            hoe::Log("step 0x2C: WARNING - screen still registered at timeout; it may linger.");
            Finish(self, "not dismissed within the configured timeout");
        }
        return;
    }
    }
}


// ===================================================================
// Speed King (Extra Chase): the one board that lost a whole VIRTUAL
// rather than a step handler. See the notes on InstallChaseHook.
// ===================================================================
namespace {

using ChaseRankFn = int (*)(void*);
ChaseRankFn s_origChaseRank = nullptr;
uintptr_t   s_chaseSlotRva  = 0;
bool        s_chasePanel    = false;
bool        s_chaseDone     = false;   // this run's panel has been dismissed
int         s_chaseValue    = 0;       // this run's PS3-encoded time, set at open
int         s_chasePrev     = 0;       // the best as it stood BEFORE this run
bool        s_chaseBest     = false;   // did this run beat it
bool        s_chaseLossLogged = false; // one LOSS line per run, not one per frame
bool        s_chaseArmed    = false;
int         s_chaseHold     = 0;
bool        s_chaseShown    = false;

// Only OUR chase, never a story one. Mission 301 is shared with seven story
// chases; 199:10 is the board's own trigger - set by Bob's row and by nothing
// else - which is the same gate PS3 uses via its row-9 flag pair.
bool ChaseIsRankingRun() {
    void* fm = game::pFlagMgr ? *game::pFlagMgr : nullptr;
    if (!fm || !game::FlagGet) return false;
    return game::FlagGet(fm, 199, 10, 1) != 0;
}

void ChaseFinish() {
    netrank::Close();
    netrank::SetNewRecord(false);
    s_chasePanel = false;
    s_chaseArmed = false;
    s_chaseShown = false;
    s_chaseHold  = 0;
    s_chaseValue = s_chasePrev = 0;
    s_chaseBest  = false;
}

}  // namespace

// CMissionMacroAdventureEscape vtable slot 88.
//
// The RETURN VALUE is the control flow: step 0x15's handler runs
//     if (!this->vf[0x2C0]()) tail-jmp this->vf[0x2C8]();
// every frame, so 1 parks the macro and re-enters us next frame, while handing
// back the original's value lets the mission end normally.
extern "C" __attribute__((ms_abi)) int HoE_ChaseRank(void* self) {
    const auto& cfg = config::Get();

    // A run is over once its panel has been dismissed; do not raise it again for
    // the same run. Without this the gate stayed true and the panel re-opened
    // immediately after being dismissed.
    if (s_chaseDone) {
        if (!game::ChaseRunFinished()) s_chaseDone = false;   // a new run started
        return s_origChaseRank ? s_origChaseRank(self) : 0;
    }

    // Not our run - hand straight back. This is the COMMON case: every story
    // chase in the game comes through here.
    // TWO conditions, and both are load-bearing.
    //   ChaseIsRankingRun  - flag 199:10, so story chases (mission 301 is shared
    //                        with seven of them) never raise the panel.
    //   ChaseRunFinished   - the run's own clock has been stopped, so the panel
    //                        cannot open on the way INTO the mode. Step 0x15 is
    //                        entered at both ends of the mission, which is what
    //                        made the first version open instantly and hang.
    if (!cfg.ChaseRankPanel ||
        (!s_chasePanel && !(ChaseIsRankingRun() && game::ChaseRunFinished()))) {
        if (!game::ChaseRunFinished()) s_chaseLossLogged = false;   // run in progress
        return s_origChaseRank ? s_origChaseRank(self) : 0;
    }

    if (!s_chasePanel) {
        // THE VERDICT DECIDES WHETHER THERE IS A PANEL AT ALL.
        //
        // The engine sits at step 0x15 polling `if (!vf88()) vf89()` until one
        // of its two verdict aliases is raised - CLEAR (1:21) about 0.4s after
        // the clock stops on a win, FAILED (1:19) about 4s after on a loss - and
        // vf88 is this slot, so we see the alias on the very frame it appears.
        //
        //   undecided -> hand back; the engine keeps polling, and so do we.
        //   FAILED    -> hand back; vf88's own body takes the fail path. No panel,
        //                no record: PS3 showed the ranking only after a success.
        //   CLEAR     -> record NOW, then open. Returning 1 parks the macro before
        //                vf89 can apply the clear script; dismissal hands back and
        //                vf89 then advances it exactly as if we had never been here.
        //
        // Nothing is latched. Two earlier versions carried the verdict across
        // frames and both went stale, because this hook only runs while the macro
        // is parked here and cannot observe the run boundary. The aliases are
        // reset by the engine at the start of every mission, so the live value is
        // always THIS run's.
        const int o = game::ReadMissionOutcome();
        if (o != 1) {
            if (o == 2 && !s_chaseLossLogged) {
                s_chaseLossLogged = true;
                hoe::Log("chase: FAILED - handing back to the engine: no panel, no record");
            }
            return s_origChaseRank ? s_origChaseRank(self) : 0;
        }

        const int frames = game::ReadChaseFrames();
        const int value  = frames > 0 ? game::RunFramesToPS3Value(frames) : 0;
        const int prev   = records::GetBest((int)off::C_MODE_SPEED_KING);
        bool best = false;
        if (cfg.TrackBestScore && value > 0) {
            best = records::SetBest((int)off::C_MODE_SPEED_KING, value, true);
        }

        netrank::SetTimeMode(true);          // board 301 is kind 2
        if (!netrank::Open()) {
            hoe::Log("chase: CLEAR but the panel would not open - ending normally "
                     "(record %s)", best ? "written" : "unchanged");
            return s_origChaseRank ? s_origChaseRank(self) : 0;
        }
        s_chasePanel = true;
        game::NoteRunResult("speed king");
        s_chaseHold  = 0;
        s_chaseArmed = false;
        s_chaseShown = false;
        s_chaseValue = value;
        s_chasePrev  = prev;
        s_chaseBest  = best;
        hoe::Log("chase: CLEAR - Speed King recap opened: %d frames -> value %d, "
                 "previous best %d%s", frames, value, prev,
                 best ? " (NEW BEST)" : (value > 0 ? " (not a best)" : ""));
        return 1;
    }

    if (netrank::Failed()) {
        hoe::Log("chase: panel build failed - ending normally");
        ChaseFinish();
        return s_origChaseRank ? s_origChaseRank(self) : 0;
    }

    if (netrank::Ready() && !s_chaseShown) {
        netrank::SetNewRecord(s_chaseBest);
        // Show the PREVIOUS best, exactly as the other boards do - the capture
        // has the old record on screen while the new one is announced.
        netrank::Show(s_chaseValue, s_chasePrev);
        s_chaseShown = true;
    }
    netrank::Draw();

    ++s_chaseHold;
    if (s_chaseHold < 2) return 1;   // one dead frame, so the press that ended
                                     // the run cannot dismiss the panel too

    const input::Pad pad = input::Read();
    if (pad.ok && !s_chaseArmed) {
        if (!input::ConfirmHeld(pad)) s_chaseArmed = true;
        return 1;
    }
    if (!(s_chaseArmed && input::ConfirmEdge(pad))) return 1;
    if (cfg.PanelSounds) game::PlaySE((unsigned)cfg.PanelSeDecide);

    hoe::Log("chase: recap dismissed after %d frames", s_chaseHold);
    ChaseFinish();
    s_chaseDone = true;
    return s_origChaseRank ? s_origChaseRank(self) : 0;
}

// Patch the SLOT, never the function: +0xB56860 is shared by eight classes
// because MSVC folded the deleted override into the base implementation.
bool InstallChaseHook() {
    if (s_origChaseRank) return true;
    const auto& r = resolve::Get();
    if (!r.AdvEscapeVft) {
        hoe::Log("chase: AdventureEscape vtable unresolved - no Speed King recap");
        return false;
    }
    s_chaseSlotRva = r.AdvEscapeVft + (uintptr_t)off::C_CHASE_RANK_SLOT * 8;
    auto slot = (uintptr_t*)(hoe::Base() + s_chaseSlotRva);
    s_origChaseRank = (ChaseRankFn)*slot;

    const uintptr_t ours = (uintptr_t)&HoE_ChaseRank;
    if (!hoe::WriteBytes(s_chaseSlotRva, &ours, sizeof(ours))) {
        hoe::Log("chase: could not write vtable slot +0x%llX",
                 (unsigned long long)s_chaseSlotRva);
        s_origChaseRank = nullptr;
        return false;
    }
    hoe::Log("chase: hooked AdventureEscape slot %d at +0x%llX (orig %p -> ours %p)",
             (int)off::C_CHASE_RANK_SLOT, (unsigned long long)s_chaseSlotRva,
             (void*)s_origChaseRank, (void*)ours);
    return true;
}


// ===================================================================
// Endless Survival Tag - the fourth board. Its macro is intact; what
// was deleted is the shared ranking mission. See the patch note.
// ===================================================================
namespace {

using SurviveStepFn = void (*)(void*);
SurviveStepFn s_origSurvive2D = nullptr;
uintptr_t     s_surviveSlotRva = 0;
bool          s_survPanel   = false;
bool          s_survArmed   = false;
bool          s_survShown   = false;
int           s_survHold    = 0;
int           s_survScore   = -1;    // latched while the tag manager is alive

// Only the ENDLESS row. "Survival Tag SP" (flag 332:6) has no ranking board and
// must keep its own result screen untouched.
bool SurviveIsRankingRun() {
    // Read the mode the way the game does at +0xB5619E, rather than re-reading
    // flag 199:1: this is the exact value step 0x21 branches on, it lives on the
    // same object the score comes from, and it does not depend on how long the
    // flag survives. 1 = ranked/Endless, 0 = plain, 2 = constructor default.
    const int mode = game::ReadTagMode();
    if (mode >= 0) return mode == 1;
    void* fm = game::pFlagMgr ? *game::pFlagMgr : nullptr;   // fallback
    if (!fm || !game::FlagGet) return false;
    return game::FlagGet(fm, 199, 1, 1) != 0;
}

void SurviveFinish() {
    netrank::Close();
    netrank::SetNewRecord(false);
    s_survPanel = false;
    s_survArmed = false;
    s_survShown = false;
    s_survHold  = 0;
    s_survScore = -1;
}

}  // namespace

// CMissionMacroAdventureSurvive step 0x2D (vtable slot 63).
//
// The original spins while the mode's own result screen is open, then notifies
// and advances to 0x2E. We keep that shape exactly and insert the ranking panel
// in the gap - which is where PS3's separate ranking mission ran.
extern "C" __attribute__((ms_abi)) void HoE_SurviveStep2D(void* self) {
    const auto& cfg = config::Get();

    // The hook used to decline in SILENCE, so a session where Endless black
    // screened produced a log with nothing but the install line. Say what the
    // gate saw, once per macro, so the next run is evidence.
    //
    // Two things here were wrong and cost a misdiagnosis, so both are fixed:
    //
    //   The verdict printed was computed from `f199_1 == 1` while the branch
    //   below actually uses SurviveIsRankingRun(). A run could therefore log
    //   "declining" and then take over, or vice versa. It now prints the same
    //   predicate the code acts on, plus the mode field that predicate reads.
    //
    //   The once-per-run guard keyed on the macro OBJECT ADDRESS, which the
    //   allocator reuses: two consecutive runs came back at the same pointer, so
    //   the second run - the one that actually took over - logged nothing at all,
    //   and the surviving line belonged to the earlier run that correctly
    //   declined. It is now keyed on a per-run counter bumped in SurviveFinish.
    {
        // Keyed on the observer's mode-end count rather than our own finish
        // path: a run this hook DECLINES never reaches SurviveFinish, so a
        // counter bumped there missed the very next run at the same address.
        // Every run is separated by a mode-end, whoever handled it.
        static void* s_lastSelf = nullptr;
        static int   s_lastRun  = -1;
        const int run = game::ModeEndCount();
        if (self != s_lastSelf || run != s_lastRun) {
            s_lastSelf = self;
            s_lastRun  = run;
            void* fm = game::pFlagMgr ? *game::pFlagMgr : nullptr;
            const int f199_1 = (fm && game::FlagGet) ? game::FlagGet(fm, 199, 1, 1) : -1;
            const int f332_6 = (fm && game::FlagGet) ? game::FlagGet(fm, 332, 6, 1) : -1;
            const bool take = cfg.EndlessTagPanel && SurviveIsRankingRun();
            hoe::Log("endless: step 0x2D reached (run %d, macro %p) - tagMode=%d "
                     "199:1=%d 332:6=%d -> %s",
                     run, self, game::ReadTagMode(), f199_1, f332_6,
                     take ? "TAKING OVER" : "declining, engine handles it");
        }
    }
    if (!cfg.EndlessTagPanel || (!s_survPanel && !SurviveIsRankingRun())) {
        if (s_origSurvive2D) s_origSurvive2D(self);
        return;
    }

    // The result screen is still up: the tag manager is still alive, so latch
    // the score NOW. Reading it after the screen closes would be a
    // use-after-free rather than just a stale value.
    void* mgr = game::pMainMgr ? *game::pMainMgr : nullptr;
    const bool screenUp =
        mgr && *(void**)((unsigned char*)mgr + off::C_MAINMGR_SURV_SCREEN) != nullptr;

    // Latch on EVERY 0x2D frame, not only while the screen is up. The manager is
    // alive for this whole step, and tying the latch to the result screen means
    // that suppressing that screen later would guarantee a permanent score of 0.
    if (s_survScore < 0) {
        const int sc = game::ReadTagScore();
        if (sc >= 0) {
            s_survScore = sc;
            hoe::Log("endless: latched score %d (result screen %s)",
                     sc, screenUp ? "up" : "not up");
        }
    }

    if (screenUp) {
        if (s_origSurvive2D) s_origSurvive2D(self);   // still spinning
        return;
    }

    // Screen closed. Instead of advancing, raise our panel and hold the step.
    if (!s_survPanel) {
        netrank::SetTimeMode(false);            // board 304 is kind 1 = SCORE
        if (!netrank::Open()) {
            hoe::Log("endless: panel would not open - advancing normally");
            SurviveFinish();
            if (s_origSurvive2D) s_origSurvive2D(self);
            return;
        }
        s_survPanel = true;
        game::NoteRunResult("endless");
        s_survHold  = 0;
        s_survArmed = false;
        s_survShown = false;
        hoe::Log("endless: Endless Survival Tag recap opened");
        return;
    }

    if (netrank::Failed()) {
        hoe::Log("endless: panel build failed - advancing normally");
        SurviveFinish();
        if (s_origSurvive2D) s_origSurvive2D(self);
        return;
    }

    if (netrank::Ready() && !s_survShown) {
        int  value = s_survScore > 0 ? s_survScore : 0;
        const int prev  = records::GetBest((int)off::C_MODE_ENDLESS_TAG);
        bool best = false;
        if (cfg.DiagForceEndlessRecord) {
            // TEST HARNESS. A real Endless run takes a long time to score; this
            // makes any run - including one ended by getting caught at once -
            // show a score of 1 and raise the NEW RECORD banner, so the banner's
            // sound can be checked in a minute rather than a session. The forced
            // value is never written to HouseOfExtras.records.
            value = 1;
            best  = true;
            hoe::Log("endless: DiagForceEndlessRecord - score forced to 1 and NEW "
                     "RECORD forced ON (real score was %d; records untouched)", s_survScore);
        } else if (cfg.TrackBestScore && value > 0) {
            best = records::SetBest((int)off::C_MODE_ENDLESS_TAG, value, false);
        }
        netrank::SetNewRecord(best);
        netrank::Show(value, prev);             // the PREVIOUS best, as elsewhere
        hoe::Log("endless: score=%d previous best=%d%s", value, prev,
                 best ? " (NEW BEST)" : "");
        s_survShown = true;
    }
    netrank::Draw();

    ++s_survHold;
    if (s_survHold < 2) return;      // one dead frame, so the press that closed
                                     // the result screen cannot dismiss us too

    const input::Pad pad = input::Read();
    if (pad.ok && !s_survArmed) {
        if (!input::ConfirmHeld(pad)) s_survArmed = true;
        return;
    }
    if (!(s_survArmed && input::ConfirmEdge(pad))) return;
    if (cfg.PanelSounds) game::PlaySE((unsigned)cfg.PanelSeDecide);

    hoe::Log("endless: recap dismissed after %d frames", s_survHold);
    SurviveFinish();
    // 0x8E0 is still null, so the original now takes its advance branch:
    // vf[0x330]() then SetStep(0x2E).
    if (s_origSurvive2D) s_origSurvive2D(self);
}

// --- Step 0x2C: suppress the engine's survival result screen for Endless ----
//
// After FixEndlessHandoff every survival-tag mode runs 0x21 -> 0x2C -> 0x2D.
// The engine's 0x2C (sub_B55520) is straight-line code:
//     refcount(+0x240, edx=1) ; TransitionSetup ; OpenScreen(223) ; SetNextStep(0x2D)
// and 0x2D then polls the screen slot at mainMgr+0x8E0 until it empties. So the
// survival result screen appeared, and only once the player closed it did our
// panel open on top - two screens where PS3 showed one.
//
// Skipping step 0x2C altogether is wrong: its refcount call with edx=1 pairs
// with an unconditional decrement in 0x2E, so routing around it drives the
// count to -1 for the session. Opening the screen and closing it at once is
// wrong too: OpenScreen registers the action synchronously and the action
// manager ticks it outside the mission tick, so one frame and the open SFX
// escape. What is safe is to let 0x2C run exactly as written with its one
// OpenScreen call turned into a 5-byte NOP for the duration of that single
// synchronous call: the slot stays empty, 0x2D sees "screen not up" on its first
// frame, and the panel opens immediately with nothing underneath it.
//
// Gated on the tag manager's mode field being 1 (Endless) - the same field the
// engine's own step 0x21 branches on - so plain Survival Tag SP (mode 0) and the
// constructor default (2) never reach the patch and keep their result screen.
//
// The NOP is self-modifying code. It is safe because mission-macro steps run on
// the game logic thread and apply / call / revert is one synchronous bracket; it
// stops being safe the day this mod executes macro code from another thread.
namespace {
SurviveStepFn s_origSurvive2C  = nullptr;
uintptr_t     s_survive2CSlot  = 0;
bool          s_survOpenSiteOk = false;   // verified at install, never assumed
hoe::Patch    s_survOpenNop;
int           s_survSuppressed = 0;       // runs suppressed so far, for the log
}  // namespace

extern "C" __attribute__((ms_abi)) void HoE_SurviveStep2C(void* self) {
    const auto& cfg = config::Get();
    const bool suppress = cfg.EndlessSuppressResult && cfg.EndlessTagPanel
                       && s_survOpenSiteOk && game::ReadTagMode() == 1;
    if (!suppress) {
        if (s_origSurvive2C) s_origSurvive2C(self);
        return;
    }
    static const unsigned char kNop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
    if (!s_survOpenNop.Apply(off::C_SURVIVE_OPENSCREEN_CALL, kNop5, sizeof kNop5,
                             "endless: skip the survival result screen")) {
        hoe::Log("endless: could not scope the OpenScreen NOP - engine screen will show");
        if (s_origSurvive2C) s_origSurvive2C(self);
        return;
    }
    if (s_origSurvive2C) s_origSurvive2C(self);
    s_survOpenNop.Revert();
    ++s_survSuppressed;
    hoe::Log("endless: survival result screen suppressed (run %d) - the NETWORK "
             "RANKING panel is the only screen, as on PS3", s_survSuppressed);
}

bool InstallSurviveStep2CHook() {
    if (s_origSurvive2C) return true;
    const auto& r = resolve::Get();
    if (!r.AdvSurviveVft || !r.OpenScreen) {
        hoe::Log("endless: AdventureSurvive vtable or OpenScreen unresolved - "
                 "result screen not suppressed");
        return false;
    }

    // The site must be exactly what we decoded: `mov edx, 223` then, 12 bytes
    // on, an E8 whose target (through the link thunk) is OpenScreen itself.
    const unsigned char* mov = (const unsigned char*)(hoe::Base() + off::C_SURVIVE_SCREEN_ID_MOV);
    static const unsigned char kMovEdx223[5] = { 0xBA, 0xDF, 0x00, 0x00, 0x00 };
    if (memcmp(mov, kMovEdx223, 5) != 0) {
        hoe::Log("endless: +0x%llX is not `mov edx, 223` - result screen not suppressed",
                 (unsigned long long)off::C_SURVIVE_SCREEN_ID_MOV);
        return false;
    }
    const unsigned char* call = (const unsigned char*)(hoe::Base() + off::C_SURVIVE_OPENSCREEN_CALL);
    if (call[0] != 0xE8) {
        hoe::Log("endless: +0x%llX is not a call - result screen not suppressed",
                 (unsigned long long)off::C_SURVIVE_OPENSCREEN_CALL);
        return false;
    }
    int32_t rel; memcpy(&rel, call + 1, 4);
    uintptr_t target = off::C_SURVIVE_OPENSCREEN_CALL + 5 + (intptr_t)rel;
    for (int hop = 0; hop < 4; ++hop) {              // follow E9 link thunks
        const unsigned char* q = (const unsigned char*)(hoe::Base() + target);
        if (q[0] != 0xE9) break;
        int32_t r2; memcpy(&r2, q + 1, 4);
        target = target + 5 + (intptr_t)r2;
    }
    if (target != r.OpenScreen) {
        hoe::Log("endless: the step-0x2C call reaches +0x%llX, not OpenScreen +0x%llX "
                 "- result screen not suppressed",
                 (unsigned long long)target, (unsigned long long)r.OpenScreen);
        return false;
    }
    s_survOpenSiteOk = true;

    s_survive2CSlot = r.AdvSurviveVft + (uintptr_t)off::C_SURVIVE_STEP2C_SLOT * 8;
    auto slot = (uintptr_t*)(hoe::Base() + s_survive2CSlot);
    s_origSurvive2C = (SurviveStepFn)*slot;
    const uintptr_t ours = (uintptr_t)&HoE_SurviveStep2C;
    if (!hoe::WriteBytes(s_survive2CSlot, &ours, sizeof ours)) {
        hoe::Log("endless: could not write vtable slot +0x%llX",
                 (unsigned long long)s_survive2CSlot);
        s_origSurvive2C = nullptr;
        return false;
    }
    hoe::Log("endless: hooked AdventureSurvive slot %d (step 0x2C) at +0x%llX "
             "(orig %p -> ours %p) - result screen suppressed for tag mode 1",
             (int)off::C_SURVIVE_STEP2C_SLOT, (unsigned long long)s_survive2CSlot,
             (void*)s_origSurvive2C, (void*)ours);
    return true;
}

bool InstallSurviveHook() {
    if (s_origSurvive2D) return true;
    const auto& r = resolve::Get();
    if (!r.AdvSurviveVft) {
        hoe::Log("endless: AdventureSurvive vtable unresolved - no panel");
        return false;
    }
    s_surviveSlotRva = r.AdvSurviveVft + (uintptr_t)off::C_SURVIVE_STEP2D_SLOT * 8;
    auto slot = (uintptr_t*)(hoe::Base() + s_surviveSlotRva);
    s_origSurvive2D = (SurviveStepFn)*slot;

    const uintptr_t ours = (uintptr_t)&HoE_SurviveStep2D;
    if (!hoe::WriteBytes(s_surviveSlotRva, &ours, sizeof(ours))) {
        hoe::Log("endless: could not write vtable slot +0x%llX",
                 (unsigned long long)s_surviveSlotRva);
        s_origSurvive2D = nullptr;
        return false;
    }
    hoe::Log("endless: hooked AdventureSurvive slot %d at +0x%llX (orig %p -> ours %p)",
             (int)off::C_SURVIVE_STEP2D_SLOT, (unsigned long long)s_surviveSlotRva,
             (void*)s_origSurvive2D, (void*)ours);
    return true;
}
