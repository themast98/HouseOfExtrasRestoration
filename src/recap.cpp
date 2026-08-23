// Replacement for CMissionMacroColosseumExtra's deleted step 0x2C handler.
//
// Sega gutted this to `ret 0` when they removed src/ranking, so the macro
// parked at step 0x2C with no next step queued -> infinite black screen.
// This mirrors CMissionMacroColosseum's intact handler (sub_B5C320), differing
// only in which screen id it opens.

#include "game.h"
#include "config.h"
#include "records.h"
#include "offsets.h"
#include "log.h"

namespace {
// Matches enemy_dispose/ene_st_tougi_8103.bin, the set Battle King uses.
constexpr int kModeBattleKing = 8103;
}

extern "C" __attribute__((ms_abi)) void HoE_Step2C(void* self) {
    const auto& cfg = config::Get();
    void* mission = *(void**)((unsigned char*)self + off::C_MISSION_FIELD);

    const int kills = game::ReadKillCount();
    if (cfg.TrackBestScore && kills >= 0) {
        const int prev = records::GetBest(kModeBattleKing);
        const bool isBest = records::SetBest(kModeBattleKing, kills);
        hoe::Log("step 0x2C: kills=%d previous best=%d%s", kills, prev, isBest ? " (NEW BEST)" : "");
    } else {
        hoe::Log("step 0x2C: kills=%d", kills);
    }

    if (!cfg.ShowResultScreen) {
        // Straight to the intact cleanup/exit step.
        game::SetNextStep(self, (int)off::C_STEP_2E);
        hoe::Log("step 0x2C: results disabled, jumping to step 0x2E");
        return;
    }

    game::TransitionSetup(mission, 1, *game::pKFloat, 0, 0);
    game::CommitFlags(*game::pFlagMgr);

    void* mgr  = *game::pMainMgr;
    void* arg3 = *(void**)((unsigned char*)mgr + off::C_OPENSCREEN_ARG3_FIELD);
    game::OpenScreen(mgr, cfg.ResultScreenId, arg3);

    game::SetNextStep(self, (int)off::C_STEP_2D);
    game::PostOpenNotify(mission, 0x1D, 0, 1, 1);

    hoe::Log("step 0x2C: opened screen %d, queued step 0x2D", cfg.ResultScreenId);
}
