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
//   done      hand off to step 0x2E (cleanup / back to Naomi's Palace)
//
// The screen manager keeps live screens in an array indexed by SCREEN ID:
//     slot(id) = mainMgr + 0x1E8 + id*8
// The engine's step 0x2D polls mainMgr+0x8F0, which is simply slot(225).
// An earlier version of this file hardcoded 0x8F0 while opening screen 220,
// so it polled a permanently-null slot and tore the stage down on the next
// frame. The slot is now computed from whichever screen id we opened.
//
// We deliberately never hand off to step 0x2D: the engine's sub_B60910 waits
// on that slot with no timeout, so routing through it could hang in a new
// place. Every wait here is ours and the open wait is bounded.

#include "game.h"
#include "config.h"
#include "records.h"
#include "offsets.h"
#include "log.h"

namespace {

// Matches enemy_dispose/ene_st_tougi_8103.bin, the set Battle King uses.
constexpr int kModeBattleKing = 8103;

// ~2s at 60fps for the screen to register. It is written synchronously inside
// OpenScreen, so this only ever catches genuine failure.
constexpr int kOpenTimeout = 120;

enum class Stage { Fresh, Opening, Showing };

void*  s_self   = nullptr;
Stage  s_stage  = Stage::Fresh;
int    s_frames = 0;

uint64_t ScreenSlot(void* mgr, int screenId) {
    const uintptr_t off = off::C_SCREEN_SLOT_BASE + (uintptr_t)screenId * 8;
    return *(uint64_t*)((unsigned char*)mgr + off);
}

void Finish(void* self, const char* why) {
    game::SetNextStep(self, (int)off::C_STEP_2E);
    hoe::Log("step 0x2C: %s -> step 0x2E (exit)", why);
    s_self = nullptr;
    s_stage = Stage::Fresh;
    s_frames = 0;
}

// Diagnostic: the kill-count field was inferred from the only `inc` in the
// kill-counter module but read back garbage at step 0x2C time. Log the pointer
// chain and a window of candidate dwords so the real field can be identified
// against a known on-screen score.
void DumpCandidates() {
    void* mgr = *game::pMainMgr;
    hoe::Log("  diag: mainMgr=%p", mgr);
    if (!mgr) return;
    auto action = *(unsigned char**)((unsigned char*)mgr + off::C_ACTION_OFFSET);
    hoe::Log("  diag: [mainMgr+0xBA0] = %p", action);
    if (!action) return;
    hoe::Log("  diag: vftable=%p", *(void**)action);
    for (uintptr_t o = 0x180; o < 0x220; o += 16) {
        hoe::Log("  diag: +0x%03llX  %10d %10d %10d %10d",
                 (unsigned long long)o,
                 *(int*)(action + o), *(int*)(action + o + 4),
                 *(int*)(action + o + 8), *(int*)(action + o + 12));
    }
}

}  // namespace

extern "C" __attribute__((ms_abi)) void HoE_Step2C(void* self) {
    const auto& cfg = config::Get();

    if (self != s_self) {                    // first frame in this step
        s_self = self;
        s_stage = Stage::Fresh;
        s_frames = 0;
    }

    void* mgr = *game::pMainMgr;
    if (!mgr) { Finish(self, "no main manager"); return; }
    const uint64_t slot = ScreenSlot(mgr, cfg.ResultScreenId);

    switch (s_stage) {

    case Stage::Fresh: {
        DumpCandidates();
        const int kills = game::ReadKillCount();
        if (cfg.TrackBestScore && kills >= 0 && kills < 100000) {
            const int prev = records::GetBest(kModeBattleKing);
            const bool best = records::SetBest(kModeBattleKing, kills);
            hoe::Log("step 0x2C: kills=%d previous best=%d%s", kills, prev, best ? " (NEW BEST)" : "");
        } else {
            hoe::Log("step 0x2C: kills=%d (implausible, not recorded)", kills);
        }

        if (!cfg.ShowResultScreen) { Finish(self, "results disabled"); return; }

        void* mission = *(void**)((unsigned char*)self + off::C_MISSION_FIELD);
        game::TransitionSetup(mission, 1, *game::pKFloat, 0, 0);
        game::CommitFlags(*game::pFlagMgr);

        void* arg3 = *(void**)((unsigned char*)mgr + off::C_OPENSCREEN_ARG3_FIELD);
        void* scr  = game::OpenScreen(mgr, cfg.ResultScreenId, arg3);
        game::PostOpenNotify(mission, 0x1D, 0, 1, 1);

        hoe::Log("step 0x2C: OpenScreen(%d) -> %p (slot mainMgr+0x%llX), parent=%p",
                 cfg.ResultScreenId, scr,
                 (unsigned long long)(off::C_SCREEN_SLOT_BASE + (uintptr_t)cfg.ResultScreenId * 8),
                 arg3);

        s_stage = Stage::Opening;
        s_frames = 0;
        return;
    }

    case Stage::Opening: {
        ++s_frames;
        if (slot != 0) {
            hoe::Log("step 0x2C: screen registered after %d frames (slot=0x%llX) - press A/B to close",
                     s_frames, (unsigned long long)slot);
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
        if (slot == 0) {                     // screen closed itself on confirm
            Finish(self, "player dismissed the results screen");
            return;
        }
        if (s_frames % 600 == 0) {
            hoe::Log("step 0x2C: results on screen, frame %d", s_frames);
        }
        // 0 = wait indefinitely, matching the original game.
        if (cfg.ResultTimeoutSec > 0 && s_frames >= cfg.ResultTimeoutSec * 60) {
            Finish(self, "results screen not dismissed within the configured timeout");
        }
        return;
    }
    }
}
