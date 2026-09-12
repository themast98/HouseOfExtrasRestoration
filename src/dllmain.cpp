#include <windows.h>
#include <string.h>
#include "netrank.h"
#include "offsets.h"
#include "patch.h"
#include "resolve.h"
#include "config.h"
#include "game.h"
#include "console.h"
#include "steamfix.h"
#include "talkdiag.h"
#include "ameba.h"
#include "log.h"

// Release version, as shown in HouseOfExtras.log and mod-meta.yaml.
#define HOE_VERSION "1.0.0"

extern "C" __attribute__((ms_abi)) void HoE_Step2C(void* self);
// Speed King's hook also lives in recap.cpp, declared here like the steps.
bool InstallChaseHook();
bool InstallSurviveHook();
bool InstallSurviveStep2CHook();   // Endless: suppress the engine's result screen
extern "C" __attribute__((ms_abi)) void HoE_Step2D(void* self);

static hoe::Patch g_patch2C;
static hoe::Patch g_patch2D;

static bool InstallPatches() {
    using namespace hoe;
    const auto& r = resolve::Get();

    // Step 0x2D: our own handler. The engine's CMissionMacroColosseum version
    // (sub_B60910) is `cmp qword [g_mainMgr+0x8F0], 0 / jne ret / edx=0x2E` -
    // it waits on screen slot(225) with no timeout, which is exactly the shape
    // PS3 used (its waiter polls screen slot 233 and only advances once the
    // player closes it). We keep the shape and change the condition, because
    // our panel is a bare layout with no screen slot to clear. Same absolute
    // jump as 0x2C: the DLL is far from the game module, and the stub has 16
    // bytes of `ret 0` + int3 padding, verified by resolve.cpp before patching.
    {
        void* target = (void*)&HoE_Step2D;
        unsigned char p[12] = { 0x48, 0xB8 };      // mov rax, imm64
        memcpy(p + 2, &target, 8);
        p[10] = 0xFF; p[11] = 0xE0;                 // jmp rax
        if (!g_patch2D.Apply(r.Step2DStub, p, sizeof(p), "step 0x2D -> HoE_Step2D")) return false;
        Log("  HoE_Step2D at %p", target);
    }

    // Step 0x2C: hand off to our own handler. Our DLL is far from the game
    // module, so this needs an absolute jump (12 bytes; 16 are available).
    {
        void* target = (void*)&HoE_Step2C;
        unsigned char p[12] = { 0x48, 0xB8 };      // mov rax, imm64
        memcpy(p + 2, &target, 8);
        p[10] = 0xFF; p[11] = 0xE0;                 // jmp rax
        if (!g_patch2C.Apply(r.Step2CStub, p, sizeof(p), "step 0x2C -> HoE_Step2C")) {
            g_patch2D.Revert();
            return false;
        }
        Log("  HoE_Step2C at %p", target);
    }
    return true;
}

// The flag manager does not exist yet at startup and is replaced wholesale
// every time a save is loaded, so a one-shot unlock would be silently undone.
// Re-applying on a slow heartbeat is three idempotent flag writes every two
// seconds - invisible next to a frame of gameplay - and it means the unlock
// survives loading a save, starting a new game, or the game's own DLC bridge
// running at any point afterwards.
static DWORD WINAPI UnlockLoop(LPVOID) {
    using namespace hoe;
    const auto& cfg = config::Get();
    bool announced = false;
    for (;;) {
        if (cfg.DiagMissionWatch) game::PollMissionState();
        game::PollExtrasReset();
        // ObserveModeEnd deliberately does NOT run here - it lives in the 2D
        // draw hook instead. See the note there: this thread is 2s granular and,
        // more importantly, is the wrong thread to be writing save flags from.
        // Read-only console from here; mutation requires the game thread.
        console::Pump(false);
        // This heartbeat also runs when only DiagMissionWatch is on, so the
        // unlock has to be gated here as well as at thread start. Without this
        // check UnlockAllModes=0 silently still unlocked - while the startup
        // log claimed the menu had been left alone - which invalidated an A/B
        // run against an unrelated crash and nearly produced a wrong verdict.
        if (cfg.UnlockAllModes && game::UnlockExtraModes() && !announced) {
            announced = true;
            if (game::IsDlcOwned) {
                Log("mode unlock applied - entitlements now owned: 0x17=%d 0x18=%d 0x19=%d",
                    (int)game::IsDlcOwned(nullptr, (int)off::C_DLC_BIT_SURVIVAL_TAG_SP),
                    (int)game::IsDlcOwned(nullptr, (int)off::C_DLC_BIT_SPEED_KING),
                    (int)game::IsDlcOwned(nullptr, (int)off::C_DLC_BIT_FASTEST_KILLER));
            }
            Log("all House of Extras modes unlocked - Bob's menu should list every row");
        }
        Sleep(2000);
    }
}

static DWORD WINAPI Init(LPVOID) {
    using namespace hoe;
    LogInit();
    Log("House of Extras Restoration " HOE_VERSION " (by Themast) attached. module base = %p, image size = 0x%llX",
        (void*)Base(), (unsigned long long)ImageSize());

    const auto& cfg = config::Get();
    if (!cfg.Enabled) { Log("Enabled=0 in HouseOfExtras.ini - doing nothing."); return 0; }

    if (!resolve::WaitForText(120000)) {
        Log("FATAL: .text never decrypted (anchor pattern never appeared).");
        return 0;
    }
    // FIRST, before anything else we do. This one is racing the Steam achievement
    // callback, which fires ~4s after launch and kills the process before our draw
    // hook has ever run. It needs nothing but a decrypted .text, so it must not
    // queue behind the RTTI scan that the rest of resolve does.
    InstallSteamAchievementFix();

    if (!resolve::Get().ok) { Log("resolve failed - NO patches installed (safe outcome)."); return 0; }
    if (!game::Bind())      { Log("game::Bind failed - NO patches installed."); return 0; }

    // Install the 2D-pass hook up front. Besides putting our draw in the render
    // phase, it is the only callback we have that runs on the GAME THREAD every
    // frame - which is what lets the console open and drive the panel anywhere
    // instead of only inside a frozen recap. That removes the arena run from
    // the panel experiment loop entirely.
    netrank::InstallDrawHook();

    // Unrelated to the recap: the survival-tag Controls panel has always drawn
    // 1.5x too large and half off-screen, because it sizes itself from the
    // output resolution rather than the 2D canvas. Nobody could see it before
    // this mod made those modes reachable.
    if (cfg.FixTagHelpPanel) game::FixTagHelpPanel();
    if (cfg.FixEndlessHandoff) game::FixEndlessHandoff();
    if (cfg.FixChaseDrainRate) game::FixChaseDrainRate();

    // Speed King lost a whole virtual rather than a step, so it needs its own
    // vtable hook rather than the step patches above.
    if (cfg.ChaseRankPanel) InstallChaseHook();
    if (cfg.DiagCaptionTrace) game::InstallCaptionTrace();   // Speed King intro diagnostics
    if (cfg.DiagTalkMatch)    talkdiag::Install();            // Bob post-mode line diagnostics
    if (cfg.AmebaFloor)       ameba::Install();               // Ameba coliseum floor (PS3 parity)

    // Endless Survival Tag's macro is intact - what was deleted is the shared
    // ranking mission - so this hooks its step 0x2D and inserts the panel where
    // that mission used to run.
    if (cfg.EndlessTagPanel) InstallSurviveHook();
    // PS3 showed only the ranking panel at the end of Endless; the engine's own
    // survival result screen is what the panel used to sit on top of.
    if (cfg.EndlessTagPanel && cfg.EndlessSuppressResult) InstallSurviveStep2CHook();

    Log(InstallPatches() ? "patches installed - House of Extras will now finish properly"
                         : "PATCH INSTALL FAILED - game left untouched");

    if (!cfg.UnlockAllModes) {
        Log("UnlockAllModes=0 - leaving Bob's menu as the PC build ships it");
    } else if (!resolve::Get().unlockOk) {
        Log("UnlockAllModes=1 but the flag setter did not resolve - menu unchanged");
    }
    // The same slow heartbeat drives the unlock and the mission watcher, so it
    // starts if either is wanted - the watcher is what makes a mode that hangs
    // diagnosable without a second play session.
    if (cfg.UnlockAllModes || cfg.DiagMissionWatch) {
        if (HANDLE t = CreateThread(nullptr, 0, UnlockLoop, nullptr, 0, nullptr)) {
            CloseHandle(t);
        } else {
            Log("could not start the background thread - unlock and watcher off");
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
        if (t) CloseHandle(t);   // thread keeps running; we just do not need the handle
    }
    return TRUE;
}
