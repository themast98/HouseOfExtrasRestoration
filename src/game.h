#pragma once
#include <stdint.h>

namespace game {

using TransitionSetupFn = void(*)(void* mission, int a2, float a3, int a4, int a5);
using CommitFlagsFn     = void(*)(void* flagMgr);
using OpenScreenFn      = void*(*)(void* mainMgr, int screenId, void* arg3);
using SetNextStepFn     = void(*)(void* self, int step);
using PostOpenNotifyFn  = void(*)(void* mission, int a2, int a3, int a4, int a5);

extern TransitionSetupFn TransitionSetup;
extern CommitFlagsFn     CommitFlags;
extern OpenScreenFn      OpenScreen;
extern SetNextStepFn     SetNextStep;
extern PostOpenNotifyFn  PostOpenNotify;
extern void**            pMainMgr;
extern void**            pFlagMgr;
extern float*            pKFloat;

bool Bind();          // fills the above from resolve::Get(); false if unresolved
int  ReadKillCount(); // CActionColosseumExtra + 0x1A4, or -1 if unavailable

}  // namespace game
