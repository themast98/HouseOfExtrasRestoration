#pragma once
#include <stdint.h>

namespace game {

using TransitionSetupFn = void(*)(void* mission, int a2, float a3, int a4, int a5);
using CommitFlagsFn     = void(*)(void* flagMgr);
using OpenScreenFn      = void*(*)(void* mainMgr, int screenId, void* arg3);
using SetNextStepFn     = void(*)(void* self, int step);
// CMissionMacroColosseumExtra vtable slot 10 (+0xB58EE0). Always called through
// the live object's own vtable, never a stored address - see CallModeTeardown.
using ModeTeardownFn    = void(*)(void* self, int kind);
using PostOpenNotifyFn  = void(*)(void* mission, int a2, int a3, int a4, int a5);
using SetResultVariantFn = void(*)(void* screen, int variant, int holdFrames);
// Decodes `alias` into a (group, bit) pair through the flag manager's own
// definition table, then sets it. Using the alias rather than a hardcoded
// (332, 30) keeps us correct even if a rebuild renumbers flag groups.
using SetFlagAliasFn     = void(*)(void* flagMgr, int alias, int value);
using FlagGetFn          = int (*)(void* flagMgr, int group, int bit, int one);
using FlagSetFn          = void(*)(void* flagMgr, int group, int bit, int value);
using IsDlcOwnedFn       = bool(*)(void* ignored, int bit);
using LayoutHasPagesFn   = int(*)(void* layout);
using LayoutPageReadyFn  = int(*)(void* layout, int page);

extern TransitionSetupFn TransitionSetup;
extern CommitFlagsFn     CommitFlags;
extern OpenScreenFn      OpenScreen;
extern SetNextStepFn     SetNextStep;
extern PostOpenNotifyFn  PostOpenNotify;
extern SetResultVariantFn SetResultVariant;
extern LayoutHasPagesFn  LayoutHasPages;
extern LayoutPageReadyFn LayoutPageReady;
extern SetFlagAliasFn    SetFlagAlias;
extern FlagGetFn         FlagGet;
extern FlagSetFn         FlagSetRaw;
extern FlagSetFn         FlagSetEdge;

// Replays Bob's own post-match reset block so a completed House of Extras mode
// can be started again. Returns true if it ran. Safe to call repeatedly.
// The running board: index 0..7, and whether it is scored by TIME.
// Re-size the survival-tag Controls panel to the 2D canvas.
bool FixTagHelpPanel();

// Route Endless Survival Tag away from the deleted ranking mission.
bool FixEndlessHandoff();

// Halve the chase stamina drain, which is tuned for PS3's 30Hz logic tick.
// One-shot: it reuses the engine's own frame-skip gate rather than measuring
// anything, so there is no per-frame counterpart to call.
bool FixChaseDrainRate();

// Per-frame, cheap: measure and report the LOGIC tick rate that the drain gate is
// computed from (g_MainMgr+0x1C), so "5 of 10 frames" can be checked against the
// rate it actually assumes rather than guessed at.
void ReportChaseDrainClock();

// Per-frame, cheap: while the live macro is CMissionMacroAdventureEscape, log its
// step and handoff fields whenever they change (bounded per run). Diagnostic for
// the intro that only plays on the first run of a process.
void TraceEscapeMacro();

// Per-frame: from the moment Bob arms Speed King (199:10 rises), log the mission
// chain the manager runs - both macro slots, any class - for ~15s.
void TraceExtrasLaunch();

// Per-frame, cheap: when Bob arms a ranked Speed King run (199:10 rises), clear
// the chase action's one-shot "intro requested" bit so Escape step 0xC broadcasts
// the title card + countdown again. Logs the byte it found, so the trace still
// shows whether the object survived from the previous run.
void ReplayChaseIntro();

// Speed King intro diagnostics (DiagCaptionTrace). The title card is
// CActionStartCaption at [g_MainMgr+0x3E8]; InstallCaptionTrace hooks the two
// AdventureEscape virtuals that drive it (slot 22 = step 4 SETS it, slot 107 =
// the step 0xE entry SHOWS it) and logs the object + every gate around each
// call. TraceStartCaption is the per-frame sampler that logs the object
// whenever one of its fields changes, so a clear/reset between runs shows up
// with a timestamp even if no hooked virtual did it.
bool InstallCaptionTrace();
void TraceStartCaption();
void ObserveCaptionSeen();   // clears 332:10 once the card has played (ChaseResetCaptionSeen)

// The engine's UI sound entry points. key = (bank << 16) | cue; the recap's
// cues are off::C_SE_KEY_*. Returns the voice handle (0 if it did not play).
// Both are no-ops that log once if the entry points did not resolve.
int  PlaySE(unsigned key);
void StopSE(int handle);

int  ReadModeIndex();
bool IsTimeMode();

// Elapsed run time in 60Hz frames (-1 on failure), and the PS3 composite
// value derived from it for display and storage.
int  ReadRunFrames();

// Extra Chase keeps its clock elsewhere - on the main manager, not the action.
int  ReadChaseFrames();

// True only once a chase run has been played AND its clock stopped.
bool ChaseRunFinished();

// The running mission's CLEAR-vs-FAIL decision, read the way the engine's own
// step-0x15 virtuals read it. 1 = CLEAR, 2 = FAILED, 0 = undecided, -1 = unreadable.
// Side-effect free, so it can be sampled every frame.
int ReadMissionOutcome();

// The Endless Survival Tag score, from the survival-tag manager.
// 1 = Endless (ranked), 0 = plain Survival Tag SP, 2 = default, -1 = unreadable.
int  ReadTagMode();

int  ReadTagScore();

// Read-only snapshot of the managers, for the startup-crash window.
void LogStartupState(int frame);
int  RunFramesToPS3Value(int frames);

bool ResetExtrasStartFlags(bool logOnly);

// Arm the reset at the end of a run; PollExtrasReset fires it once the macro is
// actually gone. Clearing DURING the recap would break the mode - sub_B60080,
// sub_B5A030, sub_B5C7A0 and sub_B60C90 all read these same flags to work out
// which mode is running.
// Watch the live macro slot; clears the start flags when a mode macro ends.
// Call every tick - it is cheap and self-gating.
void ObserveModeEnd();

// A run produced a result (a score, a time, a kill count). Called from the
// recap paths; ObserveModeEnd consumes it to tell a finished run from one the
// player quit, which decides between Bob's two post-mode lines.
void NoteRunResult(const char* who);

// How many times ObserveModeEnd has fired this session. A cheap per-run key:
// every House of Extras run is separated by one, whichever hook handled it.
int  ModeEndCount();

void ArmExtrasReset(void* extrasMacro);
void StartMissionTrace();
void PollExtrasReset();
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

// Dumps every live layout slot: name, resource flags, page count and whether
// it has a texture archive. The point is comparison - if layouts that are
// visibly on screen also have a null texture handle, then a null handle on the
// recap's slot proves nothing and the search moves elsewhere.
void LogAllLayoutSlots(const char* when);

// The layout's slot index, and how many pages the engine has published for
// it. Both are needed BEFORE calling anything that builds pages: the builder
// reads layout[8] and indexes the resource table with it unchecked, so a -1
// slot (LoadLayout found none free) walks off the front of the table, and a
// zero count makes the one-shot builder latch 'built' with no pages at all.
// Return -1 when the value cannot be read.
int LayoutSlot(void* layout);
int LayoutPageCount(void* layout);

// Samples the running mission macro (class, macro id, current and next step).
// Logs only when something changes, so a whole play session costs a handful of
// lines but a mode that parks forever names the exact class and step it stuck
// on - including modes our patch never touches.
void PollMissionState();

}  // namespace game
