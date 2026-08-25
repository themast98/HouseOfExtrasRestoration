#include "game.h"
#include "offsets.h"
#include "resolve.h"
#include "patch.h"
#include "log.h"

namespace game {

TransitionSetupFn TransitionSetup = nullptr;
CommitFlagsFn     CommitFlags     = nullptr;
OpenScreenFn      OpenScreen      = nullptr;
SetNextStepFn     SetNextStep     = nullptr;
PostOpenNotifyFn  PostOpenNotify  = nullptr;
SetResultVariantFn SetResultVariant = nullptr;
SetFlagAliasFn    SetFlagAlias    = nullptr;
IsDlcOwnedFn      IsDlcOwned      = nullptr;
void**            pMainMgr        = nullptr;
void**            pFlagMgr        = nullptr;
float*            pKFloat         = nullptr;
uint32_t*         pDlcMask        = nullptr;

bool Bind() {
    const auto& r = resolve::Get();
    if (!r.ok) return false;
    const uintptr_t b = hoe::Base();
    TransitionSetup = (TransitionSetupFn)(b + r.TransitionSetup);
    CommitFlags     = (CommitFlagsFn)    (b + r.CommitFlags);
    OpenScreen      = (OpenScreenFn)     (b + r.OpenScreen);
    SetNextStep     = (SetNextStepFn)    (b + r.SetNextStep);
    PostOpenNotify  = (PostOpenNotifyFn) (b + r.PostOpenNotify);
    SetResultVariant = (SetResultVariantFn)(b + r.SetResultVariant);
    pMainMgr        = (void**)(b + r.GMainMgr);
    pFlagMgr        = (void**)(b + r.GFlagMgr);
    pKFloat         = (float*)(b + r.KFloat);
    if (r.unlockOk) {
        SetFlagAlias = (SetFlagAliasFn)(b + r.SetFlagAlias);
        pDlcMask     = (uint32_t*)     (b + r.GDlcMask);
        if (r.IsDlcOwned) IsDlcOwned = (IsDlcOwnedFn)(b + r.IsDlcOwned);
    }
    return true;
}

bool IsColosseumExtra(const void* vft) {
    const auto& r = resolve::Get();
    if (!r.ok || !r.ActionExtraVft || !vft) return false;
    return (uintptr_t)vft == hoe::Base() + r.ActionExtraVft;
}

// mainMgr+0xBA0 is a polymorphic "current action" slot written from several
// sites, so the concrete type must be confirmed before reading fields off it.
int ReadKillCount() {
    if (!pMainMgr || !*pMainMgr) return -1;
    auto action = *(unsigned char**)((unsigned char*)*pMainMgr + off::C_ACTION_OFFSET);
    if (!action) return -1;
    if (!IsColosseumExtra(*(void**)action)) return -1;
    return *(int*)(action + off::C_KILL_COUNT_OFFSET);
}

// Bob's seven rows are a talk-script select. Four of them are conditional on
// save flags in group 332, which the game sets from DLC entitlements - but the
// PC entitlement table grants bits 0x00-0x16 and 0x1A while silently skipping
// 0x17-0x19, so those rows can never appear no matter how far you progress.
//
// Granting ownership is not cosmetic. The game's surviving bridge writes these
// very flags *from* ownership, so if it ran while the bits were still clear it
// would set them back to 0 and undo us; with ownership true it re-affirms.
bool UnlockExtraModes() {
    if (!SetFlagAlias || !pDlcMask) return false;

    static const int kDlcBits[] = {
        (int)off::C_DLC_BIT_SURVIVAL_TAG_SP,
        (int)off::C_DLC_BIT_SPEED_KING,
        (int)off::C_DLC_BIT_FASTEST_KILLER,
    };
    static const int kAliases[] = {
        (int)off::C_FLAG_ALIAS_SURVIVAL_TAG_SP,   // also gates Endless Tag
        (int)off::C_FLAG_ALIAS_SPEED_KING,
        (int)off::C_FLAG_ALIAS_FASTEST_KILLER,
    };

    uint32_t want = 0;
    for (int bit : kDlcBits) {
        if (bit >> 5) return false;   // a different dword; cannot happen today
        want |= 1u << (bit & 31);
    }
    if ((pDlcMask[0] & want) != want) pDlcMask[0] |= want;

    // The flag manager is built late and replaced wholesale on save load, so
    // until it is live there is nothing to write; the caller simply retries.
    auto mgr = (unsigned char*)(pFlagMgr ? *pFlagMgr : nullptr);
    if (!mgr || !*(void**)(mgr + off::C_FLAGMGR_TABLE_FIELD)) return false;

    for (int alias : kAliases) SetFlagAlias(mgr, alias, 1);
    return true;
}

}  // namespace game
