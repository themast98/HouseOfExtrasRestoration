// Replacement for CMissionMacroColosseumExtra's deleted step 0x2C handler.
//
// Sega gutted this to `ret 0` when they removed src/ranking, so the macro parked
// at step 0x2C with no next step queued -> infinite black screen.
//
// A step handler is invoked EVERY FRAME while it is the current step - that is
// precisely why the `ret 0` stub span forever. We use that to run the whole
// recap lifecycle here:
//
//   Fresh     open the results screen (222 = CActionSurvivalBattleResult)
//   Opening   wait for its screen slot to become non-null      (bounded)
//   Loading   caption ids only: wait for the layout, then call the engine's
//             own variant setter. A result action skips this entirely.
//   Showing   wait until the action closes itself               (bounded)
//   done      queue step 0x2E (cleanup / back to Naomi's Palace)
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
// We deliberately never hand off to step 0x2D: the engine's sub_B60910 waits on
// slot(225) with no timeout, so routing through it could hang in a new place.
// Every wait here is ours and every one of them is bounded.

#include "game.h"
#include "config.h"
#include "records.h"
#include "console.h"
#include "offsets.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>

namespace {

// Matches enemy_dispose/ene_st_tougi_8103.bin, the set Battle King uses.
constexpr int kModeBattleKing = 8103;

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

void Finish(void* self, const char* why) {
    game::SetNextStep(self, (int)off::C_STEP_2E);
    hoe::Log("step 0x2C: %s -> step 0x2E (exit)", why);
    s_stage = Stage::Fresh;
    s_frames = 0;
    s_lastTick = 0;
    s_screen = nullptr;
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

}  // namespace

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

        const int kills = game::ReadKillCount();
        if (cfg.TrackBestScore && kills > 0) {
            const int prev = records::GetBest(kModeBattleKing);
            const bool best = records::SetBest(kModeBattleKing, kills);
            hoe::Log("step 0x2C: score=%d previous best=%d%s", kills, prev, best ? " (NEW BEST)" : "");
        } else {
            hoe::Log("step 0x2C: score field unresolved (read %d), not recorded", kills);
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
