#include <windows.h>
#include <string.h>
#include "offsets.h"
#include "patch.h"
#include "resolve.h"
#include "config.h"
#include "game.h"
#include "log.h"

extern "C" __attribute__((ms_abi)) void HoE_Step2C(void* self);

static hoe::Patch g_patch2C;
static hoe::Patch g_patch2D;

static bool InstallPatches() {
    using namespace hoe;
    const auto& r = resolve::Get();

    // Step 0x2D: the engine already has a working handler for this step on
    // CMissionMacroColosseum. Reuse it verbatim with a 5-byte relative jump.
    {
        const int64_t delta = (int64_t)r.Step2DWorking - (int64_t)(r.Step2DStub + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) { Log("step 0x2D too far for rel32"); return false; }
        unsigned char p[5] = { 0xE9 };
        const int32_t rel = (int32_t)delta;
        memcpy(p + 1, &rel, 4);
        if (!g_patch2D.Apply(r.Step2DStub, p, sizeof(p), "step 0x2D -> engine handler")) return false;
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
    bool announced = false;
    for (;;) {
        if (game::UnlockExtraModes() && !announced) {
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
    Log("House of Extras ASI attached. module base = %p, image size = 0x%llX",
        (void*)Base(), (unsigned long long)ImageSize());

    const auto& cfg = config::Get();
    if (!cfg.Enabled) { Log("Enabled=0 in HouseOfExtras.ini - doing nothing."); return 0; }

    if (!resolve::WaitForText(120000)) {
        Log("FATAL: .text never decrypted (anchor pattern never appeared).");
        return 0;
    }
    if (!resolve::Get().ok) { Log("resolve failed - NO patches installed (safe outcome)."); return 0; }
    if (!game::Bind())      { Log("game::Bind failed - NO patches installed."); return 0; }

    Log(InstallPatches() ? "patches installed - House of Extras will now finish properly"
                         : "PATCH INSTALL FAILED - game left untouched");

    if (!cfg.UnlockAllModes) {
        Log("UnlockAllModes=0 - leaving Bob's menu as the PC build ships it");
    } else if (!resolve::Get().unlockOk) {
        Log("UnlockAllModes=1 but the flag setter did not resolve - menu unchanged");
    } else if (HANDLE t = CreateThread(nullptr, 0, UnlockLoop, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        Log("could not start the unlock thread - menu unchanged");
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
