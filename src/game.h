#pragma once
#include <stdint.h>

namespace game {

using TransitionSetupFn = void(*)(void* mission, int a2, float a3, int a4, int a5);
using CommitFlagsFn     = void(*)(void* flagMgr);
using OpenScreenFn      = void*(*)(void* mainMgr, int screenId, void* arg3);
using SetNextStepFn     = void(*)(void* self, int step);
using PostOpenNotifyFn  = void(*)(void* mission, int a2, int a3, int a4, int a5);
using SetResultVariantFn = void(*)(void* screen, int variant, int holdFrames);

extern TransitionSetupFn TransitionSetup;
extern CommitFlagsFn     CommitFlags;
extern OpenScreenFn      OpenScreen;
extern SetNextStepFn     SetNextStep;
extern PostOpenNotifyFn  PostOpenNotify;
extern SetResultVariantFn SetResultVariant;
extern void**            pMainMgr;
extern void**            pFlagMgr;
extern float*            pKFloat;

bool Bind();          // fills the above from resolve::Get(); false if unresolved
bool IsColosseumExtra(const void* vft);   // is this pointer's vtable CActionColosseumExtra?
int  ReadKillCount();                     // -1 when the field cannot be trusted

}  // namespace game
