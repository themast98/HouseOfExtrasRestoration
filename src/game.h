#pragma once
#include <stdint.h>

namespace game {

using TransitionSetupFn = void(*)(void* mission, int a2, float a3, int a4, int a5);
using CommitFlagsFn     = void(*)(void* flagMgr);
using OpenScreenFn      = void*(*)(void* mainMgr, int screenId, void* arg3);
using SetNextStepFn     = void(*)(void* self, int step);
using PostOpenNotifyFn  = void(*)(void* mission, int a2, int a3, int a4, int a5);
using SetResultVariantFn = void(*)(void* screen, int variant, int holdFrames);
// Decodes `alias` into a (group, bit) pair through the flag manager's own
// definition table, then sets it. Using the alias rather than a hardcoded
// (332, 30) keeps us correct even if a rebuild renumbers flag groups.
using SetFlagAliasFn     = void(*)(void* flagMgr, int alias, int value);
using IsDlcOwnedFn       = bool(*)(void* ignored, int bit);
using LayoutIsLoadingFn  = int(*)(void* layout);
using LayoutPageReadyFn  = int(*)(void* layout, int page);

extern TransitionSetupFn TransitionSetup;
extern CommitFlagsFn     CommitFlags;
extern OpenScreenFn      OpenScreen;
extern SetNextStepFn     SetNextStep;
extern PostOpenNotifyFn  PostOpenNotify;
extern SetResultVariantFn SetResultVariant;
extern SetFlagAliasFn    SetFlagAlias;
extern IsDlcOwnedFn      IsDlcOwned;
extern void**            pMainMgr;
extern void**            pFlagMgr;
extern float*            pKFloat;
extern uint32_t*         pDlcMask;

bool Bind();          // fills the above from resolve::Get(); false if unresolved
bool IsColosseumExtra(const void* vft);   // is this pointer's vtable CActionColosseumExtra?
int  ReadKillCount();                     // -1 when the field cannot be trusted

// Grants the three DLC entitlements the PC build never grants, and sets the
// save flags they gate, so all of Bob's House of Extras rows appear.
// Idempotent and cheap - safe to call repeatedly, which is how it survives a
// save load replacing the flag bank underneath us.
// Returns true once the save flags have actually been written.
bool UnlockExtraModes();

// Reports why a layout may be invisible: whether it is still loading, whether
// its page is ready, how many elements the engine registered for it, and -
// the interesting one - whether its texture archive was ever loaded. Writes to
// the log and returns false if the layout could not be inspected at all.
bool LogLayoutResources(const char* when, void* layout);

// Samples the running mission macro (class, macro id, current and next step).
// Logs only when something changes, so a whole play session costs a handful of
// lines but a mode that parks forever names the exact class and step it stuck
// on - including modes our patch never touches.
void PollMissionState();

}  // namespace game
