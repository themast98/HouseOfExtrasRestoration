// Replacement for CMissionMacroColosseumExtra's deleted step 0x2C handler.
//
// Sega gutted this to `ret 0` when they removed src/ranking, so the macro
// parked at step 0x2C with no next step queued -> infinite black screen.
//
// A step handler is invoked EVERY FRAME while it is the current step - that is
// precisely why the `ret 0` stub span forever. We use that to run the whole
// recap lifecycle here, mirroring the original console behaviour where the
// player dismisses the results screen with Cross/Circle (Xbox A/B):
//
//   Fresh     open the result screen
//   Opening   wait for its screen slot to become non-null (bounded)
//   Showing   hold while the player reads it; the screen closes ITSELF when
//             the confirm button is pressed, which nulls the slot again
//   done      queue step 0x2E (cleanup / back to Naomi's Palace)
//
// Live screens are kept in an array indexed by SCREEN ID:
//     slot(id) = mainMgr + 0x1E8 + id*8
// The engine's step 0x2D polls mainMgr+0x8F0, which is simply slot(225).
//
// We deliberately never hand off to step 0x2D: the engine's sub_B60910 waits
// on that slot with no timeout, so routing through it could hang in a new
// place. Every wait here is ours, and both are bounded.

#include "game.h"
#include "config.h"
#include "records.h"
#include "offsets.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>

namespace {

// Matches enemy_dispose/ene_st_tougi_8103.bin, the set Battle King uses.
constexpr int kModeBattleKing = 8103;

// ~2s at 60fps for the screen to register. It is written synchronously inside
// OpenScreen, so this only ever catches genuine failure.
constexpr int kOpenTimeout = 120;

// CActionColosseumExtra is allocated with size 0x218 (the allocation site does
// `mov ecx, 0x218`), so any field window must stop there.
constexpr uintptr_t kActionSize = 0x218;

// Entry detection: the handler is called every frame while step 0x2C is
// current, so a gap between calls means we have re-entered the step. Keying
// off the `self` pointer instead would resume stale state whenever the macro
// left the step without us running Finish() (e.g. quit to title mid-recap).
constexpr uint64_t kFreshGapMs = 500;

enum class Stage { Fresh, Opening, Showing };

Stage    s_stage    = Stage::Fresh;
int      s_frames   = 0;
uint64_t s_lastTick = 0;
void*    s_screen   = nullptr;

// Screen object sizes, taken from each handler's allocation site. Reading past
// these is an out-of-bounds access, so every diagnostic read is bounded by them.
size_t ScreenSize(int id) {
    if (id == 220) return 0x1C8;   // sub_333260: mov ecx, 0x1C8
    if (id == 225) return 0x220;   // sub_333A40: mov ecx, 0x220
    return 0;                      // unknown layout - read nothing
}

// What we are looking for differs per screen:
//   220: [+0x1B8] is a result-variant selector its own draw/update switch on
//        (0..3 valid; the ctor leaves 4, an uninitialised sentinel).
//   225: [+0x214] gates Draw entirely and is only set once pjs_dlc_result
//        finishes loading; [+0x1B8]/[+0x1C0] are its layout objects.
// [+8] is the shared CScreen flags word (bit 0x20 is set on close).
void LogScreenState(const char* when, int id) {
    if (!s_screen) return;
    const size_t size = ScreenSize(id);
    if (!size) { hoe::Log("  screen %s: unknown screen id %d, not probing", when, id); return; }
    auto s = (unsigned char*)s_screen;

    auto dw = [&](uintptr_t o) -> long long {
        return (o + 4 <= size) ? (long long)*(int*)(s + o) : -1LL;
    };
    auto qw = [&](uintptr_t o) -> unsigned long long {
        return (o + 8 <= size) ? *(unsigned long long*)(s + o) : 0ULL;
    };

    hoe::Log("  screen %s: flags=0x%08X variant[+1B8]=%lld lay[+1C0]=0x%llX "
             "drawgate[+214]=%lld state[+1D8]=%lld startup[+218]=%lld",
             when, (unsigned)dw(8), dw(0x1B8), qw(0x1C0), dw(0x214), dw(0x1D8), dw(0x218));
}

uint64_t ScreenSlot(void* mgr, int screenId) {
    // screenId is range-checked in config::Load(), so this stays inside the
    // slot array; the assert documents the invariant for future edits.
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

// Optional field dump used to hunt the real kill-count offset. Off by default:
// it walks a window of a live engine object and is only meaningful while
// someone is comparing it against a known on-screen score.
void DumpCandidates() {
    void* mgr = *game::pMainMgr;
    if (!mgr) { hoe::Log("  diag: no mainMgr"); return; }
    auto action = *(unsigned char**)((unsigned char*)mgr + off::C_ACTION_OFFSET);
    if (!action) { hoe::Log("  diag: no action object"); return; }

    // mainMgr+0xBA0 is a polymorphic "current action" slot written from several
    // sites, so confirm the concrete type before reading fields off it.
    const void* vft = *(void**)action;
    const bool isExtra = game::IsColosseumExtra(vft);
    hoe::Log("  diag: action=%p vftable=%p (%s)", action, vft,
             isExtra ? "CActionColosseumExtra" : "NOT CActionColosseumExtra");
    if (!isExtra) return;

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
        s_stage = Stage::Fresh;                 // re-entered the step
        s_frames = 0;
    }
    s_lastTick = now;

    void* mgr = *game::pMainMgr;
    if (!mgr) { Finish(self, "no main manager"); return; }

    switch (s_stage) {

    case Stage::Fresh: {
        if (cfg.DiagDumpFields) DumpCandidates();

        // NOTE: the kill-count field is NOT yet identified. 0x1A4 was wrong,
        // and 0x1B4 collided with a 0..7 state selector. ReadKillCount() is
        // type-checked and returns -1 when it cannot be trusted; recording is
        // off by default until the field is proven against a known score.
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

        hoe::Log("step 0x2C: OpenScreen(%d) -> %p (slot mainMgr+0x%llX), parent=%p",
                 cfg.ResultScreenId, scr,
                 (unsigned long long)(off::C_SCREEN_SLOT_BASE + (uintptr_t)cfg.ResultScreenId * 8),
                 arg3);

        if (!scr) { Finish(self, "OpenScreen returned null"); return; }

        s_screen = scr;
        if (cfg.DiagScreenState) LogScreenState("at open", cfg.ResultScreenId);
        s_stage = Stage::Opening;
        s_frames = 0;
        return;
    }

    case Stage::Opening: {
        ++s_frames;
        if (ScreenSlot(mgr, cfg.ResultScreenId) != 0) {
            hoe::Log("step 0x2C: screen registered after %d frames - press A/B to close", s_frames);
            s_stage = Stage::Showing;
            s_frames = 0;
            return;
        }
        if (s_frames >= kOpenTimeout) {
            Finish(self, "screen slot never populated (open timeout)");
        }
        return;
    }

    case Stage::Showing: {
        ++s_frames;
        if (ScreenSlot(mgr, cfg.ResultScreenId) == 0) {   // closed itself on confirm
            Finish(self, "player dismissed the results screen");
            return;
        }
        if (cfg.DiagScreenState && s_frames % 60 == 0 && s_frames <= 600) {
            char when[32];
            snprintf(when, sizeof(when), "frame %d", s_frames);
            LogScreenState(when, cfg.ResultScreenId);
        } else if (s_frames % 600 == 0) {
            hoe::Log("step 0x2C: results on screen, frame %d", s_frames);
        }
        // 0 disables the timeout. Timing out while the screen is still
        // registered leaves it on top of gameplay, which is bad - but far less
        // bad than the unbounded freeze this mod exists to remove.
        if (cfg.ResultTimeoutSec > 0 &&
            (int64_t)s_frames >= (int64_t)cfg.ResultTimeoutSec * 60) {
            hoe::Log("step 0x2C: WARNING - screen still registered at timeout; "
                     "it may linger on screen. Set ResultTimeoutSec=0 if it is dismissible.");
            Finish(self, "results screen not dismissed within the configured timeout");
        }
        return;
    }
    }
}
