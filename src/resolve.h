#pragma once
#include <stdint.h>

namespace resolve {

// Every address here is an RVA, discovered at runtime. Nothing in this project
// outside resolve.cpp may use a hardcoded address.
struct Resolved {
    bool ok = false;

    // via RTTI: CMissionMacroColosseumExtra vtable
    uintptr_t Step2CStub    = 0;   // slot 62 - patch site 1
    uintptr_t Step2DStub    = 0;   // slot 63 - patch site 2

    // via RTTI: CMissionMacroColosseum vtable
    uintptr_t RefStep2C     = 0;   // slot 62 - the handler we mirror
    uintptr_t Step2DWorking = 0;   // slot 63 - reusable working handler

    // read out of RefStep2C
    uintptr_t TransitionSetup = 0;
    uintptr_t CommitFlags     = 0;
    uintptr_t OpenScreen      = 0;
    uintptr_t SetNextStep     = 0;
    uintptr_t PostOpenNotify  = 0;
    uintptr_t GFlagMgr        = 0;
    uintptr_t GMainMgr        = 0;
    uintptr_t KFloat          = 0;

    // RTTI: CActionColosseumExtra vtable, used to type-check [mainMgr+0xBA0]
    uintptr_t ActionExtraVft  = 0;

    // Via the DLC->save-flag bridge anchor (sub_BCD251). These unlock the six
    // rows of Bob's menu that the PC build gates behind DLC entitlements it
    // never grants. Optional: if they fail to resolve, `ok` still holds and the
    // mode-completion fix installs anyway.
    uintptr_t SetFlagAlias = 0;   // sub_28A7A0(flagMgr, alias, value)
    uintptr_t IsDlcOwned   = 0;   // sub_CB5970(ignored, bit) -> bool
    uintptr_t GDlcMask     = 0;   // the 64-byte ownership bitset IsDlcOwned reads
    bool      unlockOk     = false;

    // pattern: sub_3A46D0(screen, variant, holdFrames) - the result-variant
    // setter the deleted src/ranking code called. Without it screen 220
    // constructs but its draw gate at +0x1C0 stays 0 and it renders nothing.
    uintptr_t SetResultVariant = 0;
};

// Resolves once and caches. Logs every address next to its expected value so a
// game update shows up as a visible mismatch rather than a silent miss.
const Resolved& Get();

// Poll until SteamStub has decrypted .text (the anchor becomes findable).
// .rdata - and therefore RTTI - is readable immediately, but .text is not.
bool WaitForText(int timeoutMs);

}  // namespace resolve
