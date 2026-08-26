// Replacement for CMissionMacroColosseumExtra's deleted step 0x2C handler.
//
// Sega gutted this to `ret 0` when they removed src/ranking, so the macro parked
// at step 0x2C with no next step queued -> infinite black screen.
//
// A step handler is invoked EVERY FRAME while it is the current step - that is
// precisely why the `ret 0` stub span forever. We use that to run the whole
// recap lifecycle here:
//
//   Fresh     open the results screen (220 = pjs_dlc_survivalbtl_end)
//   Opening   wait for its screen slot to become non-null      (bounded)
//   Loading   wait for the layout to finish loading, then call the engine's own
//             result-variant setter                            (bounded)
//   Showing   hold while the player reads it, until the screen closes itself
//   done      queue step 0x2E (cleanup / back to Naomi's Palace)
//
// WHY THE SETTER MATTERS. Screen 220's constructor leaves [obj+0x1C0] = 0, and
// its Draw returns immediately while that is zero. The engine's own setter
// sub_3A46D0(screen, variant, holdFrames) writes the variant to +0x1B8, the hold
// to +0x1BC, opens the draw gate at +0x1C0, AND plays the layout page's type-0
// "in" animation. That animation is what sets bit 0 of the element's flags word,
// and the element updater returns unless that bit is set - so writing the fields
// by hand would still render nothing. It also does nothing at all unless the
// variant is 0..3, which is why the constructor's 4 sentinel stays blank.
//
// ORDERING: the setter dereferences the layout page array unconditionally, so
// calling it before the layout has loaded ([obj+0x1A8] != 0) is a null deref.
// Hence the separate Loading stage.
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

// Screen object sizes, from each handler's allocation site. Every diagnostic
// read is bounded by these so the probe cannot run off the end of the object.
size_t ScreenSize(int id) {
    if (id == 220) return 0x1C8;   // sub_333260: mov ecx, 0x1C8
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

        void* mission = *(void**)((unsigned char*)self + off::C_MISSION_FIELD);
        if (cfg.CallTransitionSetup) {
            game::TransitionSetup(mission, 1, *game::pKFloat, 0, 0);
        } else {
            hoe::Log("step 0x2C: TransitionSetup skipped (CallTransitionSetup=0)");
        }
        game::CommitFlags(*game::pFlagMgr);

        void* arg3 = *(void**)((unsigned char*)mgr + off::C_OPENSCREEN_ARG3_FIELD);
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
        if (ScreenField(cfg.ResultScreenId, off::C_SCREEN_DRAWGATE_FIELD) == 0) {
            Finish(self, "banner finished (draw gate closed)");
            return;
        }
        if (cfg.DiagScreenState && s_frames % 120 == 0 && s_frames <= 600) {
            char when[32];
            snprintf(when, sizeof(when), "showing f%d", s_frames);
            LogScreenState(when, cfg.ResultScreenId);
            LogElementState(when, cfg.ResultVariant);
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
