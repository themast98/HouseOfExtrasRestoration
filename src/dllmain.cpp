#include <windows.h>
#include "offsets.h"
#include "patch.h"
#include "resolve.h"
#include "log.h"

static DWORD WINAPI Init(LPVOID) {
    using namespace hoe;
    LogInit();
    Log("House of Extras ASI attached. module base = %p, image size = 0x%llX",
        (void*)Base(), (unsigned long long)ImageSize());

    if (!resolve::WaitForText(120000)) {
        Log("FATAL: .text never decrypted (anchor pattern never appeared).");
        Log("       Either this is not the expected Yakuza 4 build, or SteamStub did not run.");
        return 0;
    }

    const auto& r = resolve::Get();
    if (!r.ok) {
        Log("resolve failed - NO patches installed (this is the safe outcome).");
        return 0;
    }

    Log("verification complete - no patches installed yet (Task 2).");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
