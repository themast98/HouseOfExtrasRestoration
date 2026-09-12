#include "game.h"
#include "offsets.h"
#include "resolve.h"
#include "patch.h"
#include "log.h"
#include "config.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace game {

TransitionSetupFn TransitionSetup = nullptr;
CommitFlagsFn     CommitFlags     = nullptr;
OpenScreenFn      OpenScreen      = nullptr;
SetNextStepFn     SetNextStep     = nullptr;
PostOpenNotifyFn  PostOpenNotify  = nullptr;
SetResultVariantFn SetResultVariant = nullptr;
SetFlagAliasFn    SetFlagAlias    = nullptr;
FlagGetFn         FlagGet         = nullptr;
FlagSetFn         FlagSetRaw      = nullptr;
FlagSetFn         FlagSetEdge     = nullptr;
IsDlcOwnedFn      IsDlcOwned      = nullptr;
LayoutHasPagesFn  LayoutHasPages  = nullptr;
LayoutPageReadyFn LayoutPageReady = nullptr;
// The RVA holds a POINTER to the resource table, so this is a void** we must
// dereference each time; the texture table at the neighbouring RVA is the
// array itself. Getting these two the same way was the original bug.
static void**         s_pLayoutRes   = nullptr;
static void**         s_layoutTexPar = nullptr;
static void**         s_pLayoutNames = nullptr;   // POINTER to the name table
static void**         s_pMissionMgr  = nullptr;
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
    if (r.FlagGet)     FlagGet     = (FlagGetFn)(b + r.FlagGet);
    if (r.FlagSetRaw)  FlagSetRaw  = (FlagSetFn)(b + r.FlagSetRaw);
    if (r.FlagSetEdge) FlagSetEdge = (FlagSetFn)(b + r.FlagSetEdge);
        pDlcMask     = (uint32_t*)     (b + r.GDlcMask);
        if (r.IsDlcOwned) IsDlcOwned = (IsDlcOwnedFn)(b + r.IsDlcOwned);
    }
    if (r.missionDiagOk) s_pMissionMgr = (void**)(b + r.GMissionMgr);
    if (r.layoutDiagOk) {
        LayoutHasPages  = (LayoutHasPagesFn) (b + r.LayoutHasPages);
        LayoutPageReady = (LayoutPageReadyFn)(b + r.LayoutPageReady);
        s_pLayoutRes    = (void**)           (b + r.GLayoutRes);
        s_layoutTexPar  = (void**)           (b + r.GLayoutTexPar);
        if (r.GLayoutNames) s_pLayoutNames = (void**)(b + r.GLayoutNames);
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

// The Extra Chase / "Speed King" run time, in 60Hz frames. -1 if unreadable.
//
// This board keeps its clock somewhere completely different from Battle King and
// Fastest Killer: not on the action at g_MainMgr+0xBA0 (nothing chase-related
// touches that offset at all), but on a CActionTimer hanging off the main
// manager itself at +0x7E8. The chase start path builds it with the action
// factory (id 0xC0 = CActionTimer) at +0x29608F, caps it at 0x57E3F frames
// (+0x2960D1) and Starts it (+0x2960DE); CActionTimer::Update then does
// `add [r8+0x1b0], eax` once per frame.
//
// THE PROOF THAT THIS IS THE RANKING VALUE: the function at +0x299340 is the
// gutted submit. It still reads this very field and still computes minutes,
// seconds and centiseconds from it -
//     +0x299440  mov ebx,[rbx+0x1B0]
//     +0x299448  call +0x3AE680      minutes = f/3600
//     +0x29944F  call +0x3AE6B0      seconds = (f/60)%60
//     +0x29945B  call +0x3AE630      centis  (capped at 0x57E3F)
//     +0x29946A  ret
// - and then discards all three, because the SetLatestTime(9, 0, m, s, centi)
// call that consumed them was deleted with src/ranking. The arithmetic Sega left
// behind is exactly what this mod has to finish.
//
// Both derefs are null-checked because the game null-checks them itself, at
// +0x299354 and +0x299364.
int ReadChaseFrames() {
    if (!pMainMgr || !*pMainMgr) return -1;
    auto timer = *(unsigned char**)((unsigned char*)*pMainMgr
                                    + off::C_MAINMGR_CHASE_TIMER);
    if (!timer) return -1;
    const int frames = *(int*)(timer + off::C_TIMER_FRAMES);
    if (frames < 0) return -1;
    const int state = *(int*)(timer + off::C_TIMER_STATE);
    if (state != 2) {
        hoe::Log("chase: timer state=%d (expected 2=stopped) - reading %d frames anyway",
                 state, frames);
    }
    return frames;
}

// Snapshot of the managers during the first seconds after the patches install.
//
// WHY THIS EXISTS. A crash in that window left a log that stopped dead at
// "patches installed" - which narrowed the fault to a ~2 second span and told us
// nothing else. The fault itself was in the game (a strlen running off an
// unterminated arena-name string) with none of our frames on the stack, so it is
// something we CORRUPT rather than something we call. These lines make the next
// occurrence say which phase it reached, instead of leaving a blank.
//
// Deliberately read-only, and every dereference is guarded: a probe that can
// itself fault would destroy the evidence it exists to collect.
void LogStartupState(int frame) {
    void* mm    = pMainMgr ? *pMainMgr : nullptr;
    void* fm    = pFlagMgr ? *pFlagMgr : nullptr;
    void* mgr   = s_pMissionMgr ? *s_pMissionMgr : nullptr;
    void* macro = nullptr;
    int   step  = -1;
    if (mgr) {
        macro = *(void**)((unsigned char*)mgr + off::C_MISSIONMGR_CURMACRO);
        if (macro) step = *(int*)((unsigned char*)macro + off::C_MACRO_STEP_FIELD);
    }
    hoe::Log("startup f%-4d mainMgr=%p flagMgr=%p missionMgr=%p macro=%p step=0x%X",
             frame, mm, fm, mgr, macro, step);
}

// Which survival-tag row is running: 1 = ranked ("Endless"), 0 = plain, 2 = the
// CSurvivalTagManager constructor default. -1 if unreadable. This is the field
// step 0x21 branches on at +0xB5619E.
int ReadTagMode() {
    if (!pMainMgr || !*pMainMgr) return -1;
    auto act = *(unsigned char**)((unsigned char*)*pMainMgr + off::C_MAINMGR_TAG_ACTION);
    if (!act) return -1;
    auto m = *(unsigned char**)(act + off::C_TAG_MANAGER_PTR);
    if (!m) return -1;
    return *(int*)(m + off::C_TAG_MODE_FIELD);
}

// The Endless Survival Tag score. -1 if unreadable.
//
// A fourth board, a fourth place to keep the value: not the action at
// g_MainMgr+0xBA0 (Battle King), not its CActionTimer (Fastest Killer), not the
// timer at g_MainMgr+0x7E8 (Extra Chase), but the survival-tag manager reached
// through g_MainMgr+0x888. The arithmetic is the game's own getter at +0xBFBCE0:
//     mov rdx,[rcx+0x1A8] / imul eax,[rdx+0x1C],0x25 / add eax,[rdx+0x18]
// reproduced here rather than called so both derefs can be null-checked - the
// manager is torn down shortly after the result screen closes.
int ReadTagScore() {
    if (!pMainMgr || !*pMainMgr) return -1;
    auto act = *(unsigned char**)((unsigned char*)*pMainMgr + off::C_MAINMGR_TAG_ACTION);
    if (!act) return -1;
    auto m = *(unsigned char**)(act + off::C_TAG_MANAGER_PTR);
    if (!m) return -1;
    const int rounds  = *(int*)(m + off::C_TAG_SCORE_MUL);
    const int inRound = *(int*)(m + off::C_TAG_SCORE_ADD);
    const int score   = rounds * 0x25 + inRound;
    // The recap reported 0 on a run that otherwise worked, and 0 is also what a
    // torn-down or freshly-reset manager reads as. Log the fields so the next run
    // says which it was instead of leaving the number unexplained.
    static int s_lastLogged = -1;
    if (score != s_lastLogged) {
        s_lastLogged = score;
        hoe::Log("tag-score: mgr=%p rounds[+0x%X]=%d inRound[+0x%X]=%d mode=%d -> %d",
                 (void*)m, (unsigned)off::C_TAG_SCORE_MUL, rounds,
                 (unsigned)off::C_TAG_SCORE_ADD, inRound,
                 *(int*)(m + off::C_TAG_MODE_FIELD), score);
    }
    return score >= 0 ? score : -1;
}
// The game's own CLEAR-vs-FAIL decision for the running mission.
//
// This replaces ReadChaseResult, which read "active" and "code" ints out of
// g_MainMgr+0x3D8 and never once succeeded: that object is CActionContinue (the
// game-over prompt), not the chase, and the two offsets it used hold floats
// there (the ctor writes 10000.0f and 0.2f). Every run logged
// "outcome active=-1 code=-1", the read failed, and the win test defaulted to
// TRUE - which is why a LOST Speed King run still wrote a record. There is no
// chase result field to find; "which code means the win" was a malformed
// question.
//
// What the engine actually uses is a pair of flag aliases in the mission
// descriptor. Step 0x15 is `if (!vf88()) tail-jmp vf89()`, and those two
// virtuals read the SAME record, differing only in which halfword pair:
//     T      = ([flagMgr+0x990] + 0x2C) + *(int32*)([flagMgr+0x990] + 0x2C)
//     CLEAR  = FlagGet(flagMgr, u16[T+0x30], u16[T+0x32], 1)   (vf88, +0xB568B6)
//     FAILED = FlagGet(flagMgr, u16[T+0x28], u16[T+0x2A], 1)   (vf89, +0xB564B2)
// Both are pure reads through the FlagGet this mod already resolves, so this can
// be sampled every frame with no side effects at all - unlike calling either
// virtual, which applies the mission op list and advances the macro.
//
// 1 = CLEAR, 2 = FAILED, 0 = not decided yet, -1 = unreadable.
int ReadMissionOutcome() {
    void* mgr = pFlagMgr ? *pFlagMgr : nullptr;
    if (!mgr || !FlagGet) return -1;
    auto tbl = *(unsigned char**)((unsigned char*)mgr + off::C_FLAGMGR_TABLE_FIELD);
    if (!tbl) return -1;
    auto base = tbl + off::C_FLAGMGR_DESC_REL;
    const int rel = *(const int*)base;
    if (!rel) return -1;
    auto T = base + rel;

    const unsigned short cg = *(unsigned short*)(T + 0x30);
    const unsigned short cb = *(unsigned short*)(T + 0x32);
    const unsigned short fg = *(unsigned short*)(T + 0x28);
    const unsigned short fb = *(unsigned short*)(T + 0x2A);
    const int clear = FlagGet(mgr, cg, cb, 1);
    const int fail  = FlagGet(mgr, fg, fb, 1);

    // The alias ids live in a runtime descriptor file, not in the exe, so log
    // them once per change. If a win ever reads FAILED, or both stay 0 through a
    // whole run, these numbers are the next lead.
    static int s_last = -2;
    const int outcome = fail ? 2 : (clear ? 1 : 0);
    if (outcome != s_last) {
        s_last = outcome;
        hoe::Log("mission-outcome: clear %u:%u=%d  fail %u:%u=%d  -> %d "
                 "(1=CLEAR 2=FAILED 0=undecided)", cg, cb, clear, fg, fb, fail, outcome);
    }
    return outcome;
}

// Has an Extra Chase run actually FINISHED?
//
// This is the gate that decides whether the recap may open, and it needs real
// semantics rather than "our virtual was called". Vtable slot 88 is reached from
// step 0x15's handler, and the macro passes through that step on the way IN as
// well as on the way out - hooking it unconditionally opened the panel the
// moment the mode started and then returned "not finished" forever, so the mode
// never began and the panel never left.
//
// The timer is unambiguous: CActionTimer::Start zeroes the frame count and sets
// state 0 (+0x3AEBF0), Update increments while state is 0, and Stop sets state 2
// (+0x3AE9B0). So "state == 2 with frames on the clock" means exactly one thing -
// a run that has been played and ended.
bool ChaseRunFinished() {
    if (!pMainMgr || !*pMainMgr) return false;
    auto timer = *(unsigned char**)((unsigned char*)*pMainMgr
                                    + off::C_MAINMGR_CHASE_TIMER);
    if (!timer) return false;
    return *(int*)(timer + off::C_TIMER_STATE) == 2
        && *(int*)(timer + off::C_TIMER_FRAMES) > 0;
}

// Which board is running, and is it scored by time or by kills?
//
// CActionColosseumExtra+0x1B4 is the board index 0..7 and +0x1B8 is nonzero for
// the time boards. The engine itself branches on +0x1B8 at +0x42E584 to choose
// between the clock layout and the kill-counter layout, so this is the game's
// own notion of the mode's kind rather than something inferred from a name.
int ReadModeIndex() {
    if (!pMainMgr || !*pMainMgr) return -1;
    auto action = *(unsigned char**)((unsigned char*)*pMainMgr + off::C_ACTION_OFFSET);
    if (!action || !IsColosseumExtra(*(void**)action)) return -1;
    return *(int*)(action + off::C_ACTION_MODE_INDEX);
}

bool IsTimeMode() {
    if (!pMainMgr || !*pMainMgr) return false;
    auto action = *(unsigned char**)((unsigned char*)*pMainMgr + off::C_ACTION_OFFSET);
    if (!action || !IsColosseumExtra(*(void**)action)) return false;
    return *(int*)(action + off::C_ACTION_TIME_MODE) != 0;
}

// The run's elapsed time, in 60 Hz frames. -1 if it cannot be read.
//
// The action owns a CActionTimer at +0x1A8; its current value is +0x1B0, which
// CActionTimer::Update increments once per frame (`add [r8+0x1b0], eax` at
// +0x3AE9A14) while its state at +0x1AC is 0. The macro stops the timer before
// handing off to our step, so by the time this runs the state should read 2.
// A value read while the timer is still running would be a frame or two long,
// so say so rather than silently reporting a slightly wrong record.
int ReadRunFrames() {
    if (!pMainMgr || !*pMainMgr) return -1;
    auto action = *(unsigned char**)((unsigned char*)*pMainMgr + off::C_ACTION_OFFSET);
    if (!action || !IsColosseumExtra(*(void**)action)) return -1;
    auto timer = *(unsigned char**)(action + off::C_ACTION_TIMER_PTR);
    if (!timer) { hoe::Log("time: the action has no timer object"); return -1; }
    const int state = *(int*)(timer + off::C_TIMER_STATE);
    const int frames = *(int*)(timer + off::C_TIMER_FRAMES);
    if (state != 2) {
        hoe::Log("time: timer state=%d (expected 2=stopped) - reading %d frames anyway",
                 state, frames);
    }
    return frames;
}

// Frames -> the composite value PS3 stores and formats. See the note at the top
// of this patch: the sub-second part is HUNDREDTHS, so V%100 recovers it exactly.
int RunFramesToPS3Value(int frames) {
    if (frames < 0) return 0;
    const int centi = (int)((float)(frames % 60) / 60.0f * 100.0f);
    return (frames / 60) * 1000 + centi;
}

// Stop Endless Survival Tag handing off to a mission that no longer exists.
//
// Step 0x21 branches on CSurvivalTagManager+8: 0 or 2 -> SetStep(0x2C), the normal
// chain; 1 (the Endless/ranked row) -> an arm that asks the mission manager to start
// mission 143 "Ranking Display" and then parks on step 0x00. The PC factory has no
// case for 143 and returns NULL, so nothing starts - and because that arm skips step
// 0x2E, the three teardown calls that restore the world never run. Black screen.
//
// Turning the `jne` at +0xB561AF into an unconditional `jmp` makes every mode take
// the 0x2C path, which is exactly what Survival Tag SP already does successfully.
// The mod's step-0x2D hook then supplies the panel mission 143 would have shown.
bool FixEndlessHandoff() {
    static hoe::Patch p;
    if (p.applied()) return true;
    const uintptr_t rva = off::C_SURVIVE_RANK_BRANCH;
    auto at = (const unsigned char*)(hoe::Base() + rva);
    if (at[0] != 0x75) {
        hoe::Log("endless-fix: +0x%llX is 0x%02X, not the expected 0x75 (jne) - skipping",
                 (unsigned long long)rva, at[0]);
        return false;
    }
    const unsigned char jmp = 0xEB;
    if (!p.Apply(rva, &jmp, 1, "endless: always take the 0x2C chain")) return false;
    hoe::Log("endless-fix: step 0x21 now routes every mode down 0x2C -> 0x2D -> 0x2E");
    return true;
}

// Halve the chase stamina drain, which is tuned for PS3's 30Hz logic tick.
//
// WHAT THE GAUGE ACTUALLY IS. There is no separate "stamina" member. At chase
// start sub_5A4570 copies a 0x48-byte record out of the chaser-type table over
// fighter+0x1CA4 and sets HP/MaxHP from it; the HUD then divides GetHp by
// GetMaxHp straight into CChaseGauge. The bar the player watches IS the
// fighter's HP.
//
// WHAT DRAINS IT. sub_5A4810, once per logic frame per live fighter (single
// caller +0x2DBE90, inside the CActionFighterManager update, gated on
// g_MainMgr+0x7D0 being a live CActionChase). It picks a flat u16 out of that
// record by movement state and passes the negation to AddHp. There is no delta
// time anywhere on the path.
//
// WHY IT IS TWICE AS FAST HERE. The PS3 EBOOT has the same routine at vaddr
// 0x234508 - verified instruction for instruction, same ability checks, same
// %10 gates, same record fields, same neg+AddHp - and chaser_type_normal.bin is
// BYTE-IDENTICAL between the two discs. Sega changed neither the code nor the
// data; only the tick rate went 30 -> 60. The player record is MaxHP 30000 with
// a top-state drain of 4/frame: 250s on PS3, 125s here.
//
// WHY NOT JUST HALVE THE NUMBERS. They are already 1/2/4 in u16. Half of 1 is
// not representable. So instead this reuses the engine's OWN frame skip: the
// ability-0x4C path computes `edi = (g_MainMgr[0x1C] % 10) < 7` and the gate at
// +0x5A48A2 skips the entire subtraction when edi is 0. NOP the ability test and
// that path becomes unconditional, at which point the threshold means "drain on
// N frames out of every 10". N=5 is PS3 parity, N=10 is stock, and N=0 turns the
// drain off completely - which is the one-chase experiment that proves this is
// really the drain before any of it is trusted.
//
// THE PREVIOUS VERSION OF THIS FUNCTION WAS WRONG AND IS NOW DELETED. It scaled
// four constants in sub_604760, which is CFighter::ApplyPendingDamage - an
// event-driven per-hit damage routine with a single conditional caller. Those
// constants are damage magnitudes. The patch applied cleanly, logged "PS3
// pacing" every second, changed the chase not at all, and quietly halved real
// combat damage for a whole session. Do not go back there.
namespace {

const unsigned char kDrainAbilityTest[4] = { 0x85, 0xC0, 0x74, 0x2A }; // test eax,eax / je
const unsigned char kDrainCmp[4]         = { 0x41, 0x83, 0xF8, 0x07 }; // cmp r8d, 7

hoe::Patch s_drainGate;
hoe::Patch s_drainThreshold;

}  // namespace

bool FixChaseDrainRate() {
    const auto& cfg = config::Get();
    if (s_drainGate.applied()) return true;

    int n = cfg.ChaseDrainFramesPer10;
    if (n < 0)  n = 0;
    if (n > 10) n = 10;
    if (n == 10) {
        hoe::Log("chase-drain: ChaseDrainFramesPer10=10 - leaving the stock 60Hz drain");
        return false;
    }

    // Both sites must still be the exact instructions we decoded, or we write
    // nothing. This is Sega's code and a game update is free to move it.
    const unsigned char* a =
        (const unsigned char*)(hoe::Base() + off::C_CHASE_DRAIN_ABILITY_TEST);
    if (memcmp(a, kDrainAbilityTest, sizeof kDrainAbilityTest) != 0) {
        hoe::Log("chase-drain: +0x%llX is %02X%02X%02X%02X, not `test eax,eax / je` "
                 "- not patching",
                 (unsigned long long)off::C_CHASE_DRAIN_ABILITY_TEST,
                 a[0], a[1], a[2], a[3]);
        return false;
    }
    const unsigned char* c =
        (const unsigned char*)(hoe::Base() + off::C_CHASE_DRAIN_THRESHOLD - 3);
    if (memcmp(c, kDrainCmp, sizeof kDrainCmp) != 0) {
        hoe::Log("chase-drain: +0x%llX is not `cmp r8d, 7` - not patching",
                 (unsigned long long)(off::C_CHASE_DRAIN_THRESHOLD - 3));
        return false;
    }

    const unsigned char nops[4] = { 0x90, 0x90, 0x90, 0x90 };
    if (!s_drainGate.Apply(off::C_CHASE_DRAIN_ABILITY_TEST, nops, sizeof nops,
                           "chase drain: always take the frame gate")) {
        return false;
    }
    const unsigned char imm = (unsigned char)n;
    if (!s_drainThreshold.Apply(off::C_CHASE_DRAIN_THRESHOLD, &imm, 1,
                                "chase drain: frames per 10")) {
        s_drainGate.Revert();
        return false;
    }

    hoe::Log("chase-drain: draining on %d of every 10 logic frames%s. "
             "This affects EVERY chase in the game, story chases included.",
             n, n == 0 ? "  <-- DRAIN DISABLED (diagnostic: if the gauge still "
                         "falls, sub_5A4810 is not the drain)"
                       : (n == 5 ? "  (PS3 parity, assuming a 60Hz logic tick)" : ""));
    return true;
}

// Measure the rate of the counter the DRAIN ITSELF gates on.
//
// N-of-10 is a halving, not a rate-independent scaler: 5/10 is exactly right at a
// 60Hz logic tick and wrong anywhere else. Whether that matters depends entirely on
// whether this game's logic tick actually varies, which has never been measured.
//
// The last attempt at this measured the mod's own 2D draw hook and called the result
// "the logic rate". It is not: the draw hook stalls during streaming, which is why
// that version kept reporting 32-50Hz dips on a machine sitting at a steady 60. This
// reads g_MainMgr+0x1C instead - the un-paused logic frame counter that sub_5A4810's
// `(counter % 10) < N` gate is computed from. It is the drain's own clock, so it
// cannot be wrong about the drain by construction.
//
// Reporting only. Nothing is retuned from this until a rate other than 60 is
// actually observed; inventing a correction for a variation that does not exist is
// how the previous two versions of this went wrong.
// Speed King's title card + countdown play on the first run of a process and never
// again, until the game is restarted.
//
// WHY. Mission macros are persistent singletons - the Escape macro is the same
// object at the same address on every run - and step 0xE (+0xB57270) decides the
// intro from a per-object field:
//     macro+0x130 == 0  ->  vf[0x2F0](); SetStep(0)     restart from the top = intro
//     macro+0x130 != 0  ->  SetStep(0x14)               skip straight to the chase
// The base constructor zeroes +0x130, so a fresh process gets the intro once. But
// the mission-END code - Escape step 0x23 (+0xB579BD) on a win and the generic
// clear finaliser (+0xB6073A) - leaves it at 1 on the object, and nothing on the
// PC path ever clears it again. Every later run inherits "already ran".
//
// THE FIX is to restore the fresh-construction value once per ranked run, at first
// sight of the macro and before step 0xE can read it. Once per run matters: after
// the intro the macro comes back at step 0x5 and passes 0xE a second time, and
// that pass MUST skip or the countdown would loop. On run 1 the field is already 0
// and this writes nothing, which doubles as the polarity check - if the intro ever
// vanished on run 1 as well, this reading would be wrong.
namespace { int s_escRun = -1; bool s_escArmed = false; int s_escLines = 0; }

// The LAUNCH trace. A fresh process runs Speed King as a chain of three missions
// (a short Escape instance, an Adventure interlude, then the Escape that runs the
// chase); every later launch runs only the last. The Escape macro's own fields are
// identical in both cases, so the divergence is upstream: in which missions the
// manager queues. This arms on the 199:10 rising edge - Bob launching the mode -
// and for the next ~15s logs, on change, whatever occupies BOTH macro slots (any
// class, by vtable RVA) plus the mission param block and queue record.
void TraceExtrasLaunch() {
    const auto& cfg = config::Get();
    if (!cfg.DiagEscapeTrace) return;
    if (!s_pMissionMgr || !*s_pMissionMgr) return;
    void* fm = pFlagMgr ? *pFlagMgr : nullptr;
    if (!fm || !FlagGet) return;
    // The flag manager exists before its definition table does. FlagGet on an
    // object without the table dereferences NULL+0xC - this trace shipped
    // without the guard and crashed the game on the first drawn frame.
    if (!*(void**)((unsigned char*)fm + off::C_FLAGMGR_TABLE_FIELD)) return;

    static bool s_seen = false; static int s_left = 0; static int s_lines = 0;
    const bool armed = FlagGet(fm, 199, 10, 1) == 1;
    if (armed && !s_seen) { s_left = 900; s_lines = 0; hoe::Log("launch-trace: 199:10 rose - tracing the mission chain"); }
    s_seen = armed;
    if (s_left <= 0) return;
    --s_left;

    auto mgr = (unsigned char*)*s_pMissionMgr;
    void* m50 = *(void**)(mgr + off::C_MISSIONMGR_CURMACRO);
    void* m58 = *(void**)(mgr + off::C_MISSIONMGR_MACRO_FIELD);
    auto vft = [](void* m) -> unsigned { return m ? (unsigned)((uintptr_t)*(void**)m - hoe::Base()) : 0u; };
    auto stp = [](void* m) -> int { return m ? *(int*)((unsigned char*)m + off::C_MACRO_STEP_FIELD) : -2; };
    auto pb  = *(unsigned char**)(hoe::Base() + off::C_G_MISSION_PARAMBLK);
    auto mq  = *(unsigned char**)(hoe::Base() + off::C_G_MISSION_QUEUE);
    const int p10 = pb ? *(int*)(pb + 0x10) : -1, p14 = pb ? *(int*)(pb + 0x14) : -1;
    const int p2c = pb ? *(int*)(pb + 0x2C) : -1, p3c = pb ? *(int*)(pb + 0x3C) : -1;
    const int q3c = mq ? *(int*)(mq + 0x3C) : -1;
    // The chase action itself and its flags byte. Escape step 0xC calls a
    // once-only latch on it: `if (!(chase+0x782 & 0x20)) { |= 0x20; start intro }`.
    auto chase = (pMainMgr && *pMainMgr) ? *(unsigned char**)((unsigned char*)*pMainMgr + off::C_MAINMGR_CHASE_ACTION) : nullptr;
    const int c782 = chase ? *(unsigned char*)(chase + off::C_CHASE_FLAGS_782) : -1;

    static unsigned lv50 = 1, lv58 = 1; static int ls50 = -9, ls58 = -9, l10 = -9, l14 = -9, l2c = -9, l3c = -9, lq = -9, lc = -9; static void* lch = (void*)1;
    const unsigned v50 = vft(m50), v58 = vft(m58); const int s50 = stp(m50), s58 = stp(m58);
    if (s_lines < 120 && (v50 != lv50 || v58 != lv58 || s50 != ls50 || s58 != ls58 || p10 != l10 || p14 != l14 || p2c != l2c || p3c != l3c || q3c != lq || c782 != lc || (void*)chase != lch)) {
        lv50 = v50; lv58 = v58; ls50 = s50; ls58 = s58; l10 = p10; l14 = p14; l2c = p2c; l3c = p3c; lq = q3c; lc = c782; lch = chase; ++s_lines;
        hoe::Log("launch-trace: +0x50 vft=+0x%X step=%d | +0x58 vft=+0x%X step=%d | chase=%p +0x782=0x%02X | parambk +10=%d +14=%d +2C=%d +3C=0x%X | queue+3C=0x%X",
                 v50, s50, v58, s58, (void*)chase, c782 < 0 ? 0xFF : c782, p10, p14, p2c, (unsigned)p3c, (unsigned)q3c);
    }
}

// The title card + countdown are not part of the mission chain at all: Escape
// step 0xC (+0xB576E0) calls +0x299B70 on the chase action, which is
//     if (chase+0x782 & 0x20) return;  chase+0x782 |= 0x20;  broadcast vf[0x78]
// to every child in chase+0x760 - a once-per-object latch. The only writer that
// clears the bit is the CActionChase constructor (+0x29596B). PS3 has the
// identical latch (EBOOT 0x5DB278, field +1698 bit 0x04 - the same bitfield,
// MSB-packed) and the same single clearing site (its ctor at 0x5D5F6C); it
// replays the intro because it rebuilds the action for every run. PC keeps the
// object, so run 2 finds the bit set and never broadcasts. Clearing it once per
// ranked run restores the PS3 behaviour without touching the mission chain.
void ReplayChaseIntro() {
    const auto& cfg = config::Get();
    if (!cfg.ChaseReplayIntro) return;
    void* fm = pFlagMgr ? *pFlagMgr : nullptr;
    if (!fm || !FlagGet) return;
    if (!*(void**)((unsigned char*)fm + off::C_FLAGMGR_TABLE_FIELD)) return;   // see TraceExtrasLaunch

    static bool s_seen = false; static int s_run = 0;
    const bool armed = FlagGet(fm, 199, 10, 1) == 1;
    const bool rose = armed && !s_seen;
    s_seen = armed;
    if (!rose) return;
    ++s_run;

    auto chase = (pMainMgr && *pMainMgr) ? *(unsigned char**)((unsigned char*)*pMainMgr + off::C_MAINMGR_CHASE_ACTION) : nullptr;
    if (!chase) { hoe::Log("chase-intro: run %d: no chase action at arm time - nothing to clear (fresh object expected)", s_run); return; }
    unsigned char* flags = chase + off::C_CHASE_FLAGS_782;
    const unsigned char was = *flags;
    if (!(was & off::C_CHASE_INTRO_BIT)) { hoe::Log("chase-intro: run %d: chase=%p +0x782=0x%02X - intro latch already clear", s_run, (void*)chase, was); return; }
    *flags = (unsigned char)(was & ~off::C_CHASE_INTRO_BIT);
    hoe::Log("chase-intro: run %d: chase=%p +0x782 was 0x%02X -> 0x%02X (intro latch cleared, title card + countdown should replay)",
             s_run, (void*)chase, was, *flags);
}

// ---- Speed King intro: CActionStartCaption diagnostics ----------------------
//
// What the player sees (black fade, brush-font title card, jingle 0x3000C, the
// 3-2-1 countdown) is CActionStartCaption, action slot 64 at [g_MainMgr+0x3E8].
//   set : Escape step 4 (+0xB55A90, slot 22) -> +0x39E560(caption): looks up the
//         CURRENT sub-mission (+0xCB9A60 on the sub-mission manager, -1 = none),
//         then Set(name, subName, id) loads the title DDS into +0x1B0 and the
//         resolved id into +0x1D0. Gates that silently skip it: Set's
//         consume-once guard +0x1D4, an empty name, an unresolvable id, no
//         current sub-mission - and step 4's own early return when BOTH
//         macro+0x130 == 0 and paramblk+0x44 == 0.
//   show: the step 0xE entry (+0xB53E80, slot 107) -> +0xB4DF00 -> Show(mode):
//         needs +0x1B0 != 0 and +0x1D0 != -1, else draws nothing and plays
//         nothing. The 4.1s window is fixed (240+8 frames) so the run looks the
//         same either way - which is exactly what the video shows on run 2.
// The trace logs all of those around both calls, so one two-run session says
// which gate fails on the second run. PS3 has the identical structure
// (0x6D5D58 Set with the same +284 guard, 0x6D5290 Show) and replays every run.
namespace {
using StepFn = void* (*)(void*, void*, void*, void*);
StepFn    s_origCapStep4 = nullptr;
StepFn    s_origCapShow  = nullptr;
uintptr_t s_capSlot4Rva  = 0;
uintptr_t s_capSlot107Rva = 0;
int       s_capLines     = 0;
int       s_capRun       = 0;
bool      s_capArmedSeen = false;

using CurSubFn = int (*)(void* mgr);

unsigned char* CaptionObj() {
    if (!pMainMgr || !*pMainMgr) return nullptr;
    return *(unsigned char**)((unsigned char*)*pMainMgr + off::C_MAINMGR_START_CAPTION);
}

int CurrentSubMission() {
    void* mgr = *(void**)(hoe::Base() + off::C_G_SUBMISSION_MGR);
    if (!mgr) return -2;
    return ((CurSubFn)(hoe::Base() + off::C_SUBMISSION_CURRENT))(mgr);
}

// Everything the gates read, in one line. `full` adds the slow bits (the
// sub-mission lookup) that only matter around the hooked calls.
void CaptionSnapshot(char* out, size_t n, bool full) {
    auto cap = CaptionObj();
    if (!cap) { snprintf(out, n, "caption=NULL"); return; }
    char name[32] = {};
    for (int i = 0; i < 31; ++i) {
        const unsigned char ch = cap[off::C_CAPTION_NAME + i];
        if (ch < 0x20 || ch > 0x7E) break;
        name[i] = (char)ch;
    }
    auto pb = *(unsigned char**)(hoe::Base() + off::C_G_MISSION_PARAMBLK);
    const int blk44 = pb ? *(int*)(pb + off::C_MISSION_PARAMBLK_SETUP) : -2;
    const int hold  = (pMainMgr && *pMainMgr) ? *(int*)((unsigned char*)*pMainMgr + off::C_MAINMGR_CAPTION_HOLD) : -2;
    int used = snprintf(out, n,
        "cap=%p sub=%p title=%p layout=%p flags=0x%X sm=%d page=%d pend=%d aux=%d mute=%d rst=%d keep=%d name='%s' | blk+44=%d hold988=%d",
        (void*)cap,
        *(void**)(cap + off::C_CAPTION_SUB_TEX), *(void**)(cap + off::C_CAPTION_TITLE_TEX),
        *(void**)(cap + off::C_CAPTION_LAYOUT),
        *(unsigned*)(cap + off::C_CAPTION_FLAGS), *(int*)(cap + off::C_CAPTION_SUBMISSION),
        *(int*)(cap + off::C_CAPTION_PAGE), *(int*)(cap + off::C_CAPTION_PENDING),
        *(int*)(cap + off::C_CAPTION_AUX), *(int*)(cap + off::C_CAPTION_MUTE),
        *(int*)(cap + off::C_CAPTION_RESET_REQ), *(int*)(cap + off::C_CAPTION_KEEP),
        name, blk44, hold);
    if (full && used > 0 && (size_t)used < n)
        snprintf(out + used, n - used, " cursub=%d", CurrentSubMission());
}

void CaptionLog(const char* what, void* macro, bool full) {
    if (s_capLines >= 400) return;
    ++s_capLines;
    char buf[400];
    CaptionSnapshot(buf, sizeof buf, full);
    const int f130 = macro ? *(int*)((unsigned char*)macro + off::C_MACRO_INTRO_DONE) : -2;
    const int step = macro ? *(int*)((unsigned char*)macro + off::C_MACRO_STEP_FIELD) : -2;
    hoe::Log("caption-trace: run %d %s macro=%p step=0x%X +0x130=%d | %s", s_capRun, what, macro, step, f130, buf);
}
}  // namespace

extern "C" __attribute__((ms_abi)) void* HoE_CaptionStep4(void* self, void* a, void* b, void* c) {
    CaptionLog("step4 IN ", self, true);
    void* r = s_origCapStep4 ? s_origCapStep4(self, a, b, c) : nullptr;
    CaptionLog("step4 OUT", self, true);
    return r;
}

extern "C" __attribute__((ms_abi)) void* HoE_CaptionShow(void* self, void* a, void* b, void* c) {
    CaptionLog("show  IN ", self, false);
    void* r = s_origCapShow ? s_origCapShow(self, a, b, c) : nullptr;
    CaptionLog("show  OUT", self, false);
    return r;
}

bool InstallCaptionTrace() {
    if (!config::Get().DiagCaptionTrace) return false;
    if (s_origCapStep4) return true;
    const auto& r = resolve::Get();
    if (!r.AdvEscapeVft) { hoe::Log("caption-trace: AdventureEscape vtable unresolved - not installed"); return false; }
    s_capSlot4Rva   = r.AdvEscapeVft + (uintptr_t)off::C_ESCAPE_STEP4_SLOT * 8;
    s_capSlot107Rva = r.AdvEscapeVft + (uintptr_t)off::C_ESCAPE_INTRO_ENTRY_SLOT * 8;
    s_origCapStep4 = (StepFn)*(uintptr_t*)(hoe::Base() + s_capSlot4Rva);
    s_origCapShow  = (StepFn)*(uintptr_t*)(hoe::Base() + s_capSlot107Rva);
    const uintptr_t ours4 = (uintptr_t)&HoE_CaptionStep4, ours107 = (uintptr_t)&HoE_CaptionShow;
    if (!hoe::WriteBytes(s_capSlot4Rva, &ours4, sizeof ours4)) {
        hoe::Log("caption-trace: could not write slot 22"); s_origCapStep4 = nullptr; s_origCapShow = nullptr; return false;
    }
    if (!hoe::WriteBytes(s_capSlot107Rva, &ours107, sizeof ours107)) {
        hoe::Log("caption-trace: could not write slot 107 (slot 22 stays hooked)"); s_origCapShow = nullptr;
    }
    hoe::Log("caption-trace: hooked AdventureEscape slot 22 (+0x%llX, orig +0x%llX) and slot 107 (+0x%llX, orig +0x%llX)",
             (unsigned long long)s_capSlot4Rva, (unsigned long long)((uintptr_t)s_origCapStep4 - hoe::Base()),
             (unsigned long long)s_capSlot107Rva, s_origCapShow ? (unsigned long long)((uintptr_t)s_origCapShow - hoe::Base()) : 0ull);
    return true;
}

// Per frame: log the caption object whenever any of its fields change. Keyed
// on the 199:10 rising edge (ModeEndCount stayed 0 across both runs in the
// 2026-09-09 log, so it is not a usable run key here).
void TraceStartCaption() {
    if (!config::Get().DiagCaptionTrace) return;
    void* fm = pFlagMgr ? *pFlagMgr : nullptr;
    if (fm && FlagGet && *(void**)((unsigned char*)fm + off::C_FLAGMGR_TABLE_FIELD)) {
        const bool armed = FlagGet(fm, 199, 10, 1) == 1;
        if (armed && !s_capArmedSeen) { ++s_capRun; CaptionLog("199:10 rose", nullptr, true); }
        s_capArmedSeen = armed;
    }
    auto cap = CaptionObj();
    if (!cap) return;
    struct Key { void *a, *b, *c; unsigned f; int sm, pg, pd, ax, mu, rs, kp, blk, hold; };
    auto pb = *(unsigned char**)(hoe::Base() + off::C_G_MISSION_PARAMBLK);
    Key k = { *(void**)(cap + off::C_CAPTION_SUB_TEX), *(void**)(cap + off::C_CAPTION_TITLE_TEX), *(void**)(cap + off::C_CAPTION_LAYOUT),
              *(unsigned*)(cap + off::C_CAPTION_FLAGS), *(int*)(cap + off::C_CAPTION_SUBMISSION), *(int*)(cap + off::C_CAPTION_PAGE),
              *(int*)(cap + off::C_CAPTION_PENDING), *(int*)(cap + off::C_CAPTION_AUX), *(int*)(cap + off::C_CAPTION_MUTE),
              *(int*)(cap + off::C_CAPTION_RESET_REQ), *(int*)(cap + off::C_CAPTION_KEEP),
              pb ? *(int*)(pb + off::C_MISSION_PARAMBLK_SETUP) : -2,
              (pMainMgr && *pMainMgr) ? *(int*)((unsigned char*)*pMainMgr + off::C_MAINMGR_CAPTION_HOLD) : -2 };
    static Key last = { (void*)1, (void*)1, (void*)1, 0xFFFFFFFFu, -9, -9, -9, -9, -9, -9, -9, -9, -9 };
    if (memcmp(&k, &last, sizeof k) == 0) return;
    last = k;
    // The macro, if the Escape is live, for the step column.
    void* macro = nullptr;
    if (s_pMissionMgr && *s_pMissionMgr) {
        const auto& r = resolve::Get();
        auto mgr = (unsigned char*)*s_pMissionMgr;
        void* inner = *(void**)(mgr + off::C_MISSIONMGR_MACRO_FIELD);
        void* outer = *(void**)(mgr + off::C_MISSIONMGR_CURMACRO);
        auto isEsc = [&](void* p) { return p && (uintptr_t)*(void**)p - hoe::Base() == r.AdvEscapeVft; };
        macro = isEsc(inner) ? inner : (isEsc(outer) ? outer : nullptr);
    }
    CaptionLog("sample   ", macro, false);
}

// Speed King intro on EVERY run: forget the "caption seen" flag the moment the
// card has set it.
//
// caption.bin entry 130 (the SPEED KING CHAMPIONSHIP card) is current only while
// 199:10 == 1 AND 332:10 == 0, and its completion op sets 332:10 = 1 when the
// card ends. Retail clears 332:10 from Bob's reset block, which PC never reaches.
// The first shape of this fix cleared it from the mode-end reset, and the
// 2026-09-09 13:51 log showed that reset never latching in the player's flow
// (the run was over before its clock was seen stopped) while the ENGINE still
// dropped and re-raised 199:10 between runs - so run 2 was armed with 332:10
// still 1. Clearing here is keyed on STATE, not on an event: whenever the bank
// reads 332:10 == 1 and no card is on screen, the card has been seen and the
// flag has done its one job. Its only reader is entry 130's condition, evaluated
// when 199:10 is raised, so this cannot fire early: FlagSetRaw(...,0) never
// enters the trigger path (+0xBF8AAF runs only for value != 0).
//
// Also samples the flags around the mode so the log shows every transition
// (who cleared 199:10, whether the abort pair 332:28/29 ever rises).
void ObserveCaptionSeen() {
    const auto& cfg = config::Get();
    if (!cfg.ChaseResetCaptionSeen && !cfg.DiagCaptionTrace) return;
    void* fm = pFlagMgr ? *pFlagMgr : nullptr;
    if (!fm || !FlagGet || !*(void**)((unsigned char*)fm + off::C_FLAGMGR_TABLE_FIELD)) return;

    if (cfg.DiagCaptionTrace) {
        struct F { short g, b; const char* n; };
        static const F kWatch[] = {
            { 199, 10, "NR_ExtraChase" }, { 332, 9, "bakuso_9" }, { 332, 10, "caption_seen" },
            { 332, 11, "bakuso_11" }, { 332, 28, "extra_abort" }, { 332, 29, "extra_end" },
            { 1, 36, "ranking_display" },
        };
        static int last[sizeof kWatch / sizeof kWatch[0]];
        static bool primed = false;
        static int lines = 0;
        for (size_t i = 0; i < sizeof kWatch / sizeof kWatch[0]; ++i) {
            const int v = FlagGet(fm, kWatch[i].g, kWatch[i].b, 1);
            if (primed && v != last[i] && lines < 200) {
                ++lines;
                hoe::Log("flag-sample: %d:%d (%s) %d -> %d", kWatch[i].g, kWatch[i].b, kWatch[i].n, last[i], v);
            }
            last[i] = v;
        }
        primed = true;
    }

    if (!cfg.ChaseResetCaptionSeen || !FlagSetRaw || !FlagSetEdge) return;
    if (FlagGet(fm, 332, 10, 1) != 1) return;
    // NOT while 199:10 is still up. The current sub-mission is COMPUTED, not
    // stored (+0xCB9A60 walks every caption entry and returns the first whose
    // condition holds), and EVERY non-zero flag write re-derives the caption
    // from it once the sub-mission manager is idle (+0xCBA360 tail-calls
    // +0x39E560 when mgr+0x44 == 0). Clearing 332:10 the instant the card ended
    // (the 2026-09-10 11:31 build) let the abort pair 332:28/29 - or the
    // win/lose verdict writes - land after the run with 199:10 still 1 and
    // 332:10 already 0, so entry 130 matched again and the palace macro's own
    // show step (Adventure slot 30 -> +0xB4DF00) played the card in the palace.
    // Once 199:10 is 0 the entry cannot match whatever 332:10 reads, so this is
    // the earliest safe moment - and it is where Bob's reset block sits on PS3:
    // between runs, before the row raises 199:10 again.
    if (FlagGet(fm, 199, 10, 1) != 0) return;
    // Bank first, then the pending list - same order and same reason as
    // ResetExtrasStartFlags.
    FlagSetRaw (fm, 332, 10, 0);
    FlagSetEdge(fm, 332, 10, 0);
    hoe::Log("caption-seen: 332:10 was 1 (the title card has played) and 199:10 is 0 (mode disarmed) - cleared, now reads %d, so the next Speed King run shows it again",
             FlagGet(fm, 332, 10, 1));
}

void TraceEscapeMacro() {
    const auto& cfg = config::Get();
    if (!cfg.DiagEscapeTrace && !cfg.EscapeReplayIntro) return;
    if (!s_pMissionMgr || !*s_pMissionMgr) return;
    const auto& r = resolve::Get();
    if (!r.AdvEscapeVft) return;
    auto mgr = (unsigned char*)*s_pMissionMgr;
    // BOTH slots. The intro and countdown steps (0x5..0xE) run on the object
    // referenced through +0x58; +0x50 only ever showed the Escape at 0x15 / 0 /
    // -1. The first version of this read +0x50 alone, never saw a single intro
    // step, and its write landed on nothing - "already 0" on every run.
    auto isEscape = [&](void* p) {
        return p && (uintptr_t)*(void**)p - hoe::Base() == r.AdvEscapeVft;
    };
    void* inner = *(void**)(mgr + off::C_MISSIONMGR_MACRO_FIELD);   // +0x58
    void* outer = *(void**)(mgr + off::C_MISSIONMGR_CURMACRO);      // +0x50
    void* cur = isEscape(inner) ? inner : (isEscape(outer) ? outer : nullptr);
    if (!cur) return;
    const int slotOff = (cur == inner) ? 0x58 : 0x50;

    auto m = (unsigned char*)cur;
    const int step  = *(int*)(m + off::C_MACRO_STEP_FIELD);
    const int f130  = *(int*)(m + off::C_MACRO_INTRO_DONE);
    const int state = *(int*)(m + 0x18);
    const int cd    = *(int*)(m + 0x164);
    const int f140  = *(int*)(m + 0x140);
    const int f150  = *(int*)(m + 0x150);

    // Runs are separated by a mode-end, so its count is the per-run key - the
    // macro address is useless for that, being identical every run.
    const int run = ModeEndCount();
    if (run != s_escRun) { s_escRun = run; s_escArmed = false; s_escLines = 0; }

    if (cfg.EscapeReplayIntro && !s_escArmed && step < 0xE) {
        s_escArmed = true;
        void* fm = pFlagMgr ? *pFlagMgr : nullptr;
        const bool ranked = fm && FlagGet && FlagGet(fm, 199, 10, 1) == 1;
        if (ranked) {
            hoe::Log("escape-intro: run %d first seen at step 0x%X with +0x130=%d -> %s",
                     run, step, f130,
                     f130 ? "cleared, so the title card and countdown play"
                          : "already 0, nothing to do (first run of the process)");
            if (f130) *(int*)(m + off::C_MACRO_INTRO_DONE) = 0;
        }
    }

    static int ls = -1, lf = -1, lst = -1, lcd = -1, l40 = -1, l50 = -1; static void* lm = nullptr;
    if (cfg.DiagEscapeTrace && s_escLines < 60 &&
        (step != ls || f130 != lf || state != lst || cd != lcd || f140 != l40 || f150 != l50 || cur != lm)) {
        ls = step; lf = f130; lst = state; lcd = cd; l40 = f140; l50 = f150; lm = cur; ++s_escLines;
        hoe::Log("escape-trace: run %d [+0x%02X %p] step=0x%X state=%d +0x130=%d +0x140=%d +0x150=%d countdown=%d",
                 run, slotOff, cur, step, state, f130, f140, f150, cd);
    }
}

// The recap's sounds, restored from the PS3 ranking code (EBOOT 0x698C00..0x698F00):
//     roll-up loop   0x10023   played with a kept handle when the number starts moving
//     settle         0x10024   after the loop is stopped, when the number lands
//     NEW RECORD     0x1000C   the instant page 9 goes up with variant (0,4)
//     confirm        0x10001   on the X/O close
// All four are bank-1 (system UI) cues that still exist in the PC banks; the key
// numbering is shared between the two platforms (verified: identical top-cue
// distributions on PS3 0xE2FF8 and PC +0xCD9580). The entry point on PC is
//     SndPlay(key, int* outHandle, flags, category) -> handle
// with category 0x12 at every UI call site. The mod itself never played a sound
// before this; the banner was silent on every board.
namespace {
using SndPlayFn = int  (*)(unsigned key, int* outHandle, unsigned flags, int category);
using SndStopFn = void (*)(int handle, int fadeFrames);
bool s_seWarned = false;
}

int PlaySE(unsigned key) {
    const auto& r = resolve::Get();
    if (!r.SndPlay) {
        if (!s_seWarned) { s_seWarned = true; hoe::Log("se: SndPlay unresolved - the recap stays silent"); }
        return 0;
    }
    auto fn = (SndPlayFn)(hoe::Base() + r.SndPlay);
    int handle = -1;
    const int ret = fn(key, &handle, 0, (int)off::C_SND_CATEGORY_UI);
    hoe::Log("se: play key 0x%X -> handle %d (ret %d)", key, handle, ret);
    return handle;
}

void StopSE(int handle) {
    const auto& r = resolve::Get();
    if (!r.SndStop || handle < 0) return;
    ((SndStopFn)(hoe::Base() + r.SndStop))(handle, 0);
}

void ReportChaseDrainClock() {
    if (!config::Get().DiagDrainClock) return;
    if (!pMainMgr || !*pMainMgr) return;

    static LARGE_INTEGER s_freq{};
    static LARGE_INTEGER s_mark{};
    static unsigned s_lastFrame = 0;
    static double s_lastHz = 0.0;

    auto mm = (const unsigned char*)*pMainMgr;
    const unsigned frame = *(const unsigned*)(mm + off::C_MAINMGR_LOGIC_FRAME);
    if (!s_freq.QuadPart) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_mark);
        s_lastFrame = frame;
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // The counter FREEZES while the game is paused - the increment at +0x329402
    // is skipped whenever these bits are set - so a window containing any pause
    // divides frozen ticks by running wall-clock and reads as "slow". That is
    // every one of the 43-58Hz MISMATCH lines the first version logged during
    // loads and menus. Any paused frame discards the window entirely.
    if (*(const unsigned*)(mm + off::C_MAINMGR_PAUSE_BITS) & off::C_MAINMGR_PAUSE_MASK) {
        s_mark = now;
        s_lastFrame = frame;
        return;
    }
    const double secs = double(now.QuadPart - s_mark.QuadPart) / double(s_freq.QuadPart);
    if (secs < 2.0) return;                       // a slow, cheap sample

    const unsigned ticks = frame - s_lastFrame;   // wraps correctly on unsigned
    s_mark = now;
    s_lastFrame = frame;
    if (!ticks) return;                           // paused: the counter is frozen

    const double hz = ticks / secs;
    // Only speak up when the answer changes meaningfully, so a steady machine logs
    // this roughly once and then goes quiet.
    if (s_lastHz > 0.0 && hz > s_lastHz * 0.95 && hz < s_lastHz * 1.05) return;
    s_lastHz = hz;

    const int ideal = (int)(10.0 * 30.0 / hz + 0.5);
    hoe::Log("drain-clock: logic tick %.1fHz (g_MainMgr+0x1C). "
             "ChaseDrainFramesPer10=%d is in use; PS3 parity at this rate would be %d%s",
             hz, config::Get().ChaseDrainFramesPer10, ideal > 10 ? 10 : ideal,
             (ideal > 10 ? 10 : ideal) == config::Get().ChaseDrainFramesPer10
                 ? "  (matching)" : "  <-- MISMATCH");
}


// Make the survival-tag Controls panel fit the screen.
//
// See the patch note: the function sizes itself from GetRT(MAIN_2D) = the
// unscaled output resolution, while the 2D canvas is a fixed 1280x720, so
// everything comes out 1920/1280 = 1.5x too large and runs off the screen.
// Each of the four call sites is 17 bytes and leaves its result in xmm0:
//     BA 02 00 00 00        mov edx, 2            (eRT_INDEX_MAIN_2D)
//     48 8B 0D d32          mov rcx, [rip+global]
//     E8 d32                call getter
// which an 8-byte `vmovss xmm0,[rip+K]` plus 9 NOPs replaces exactly. Both the
// site and the constant are in the image, so the displacement is
// base-independent and computed from RVAs alone.
//
// The dimmer quad's own four sites are deliberately left alone - oversized it
// merely over-covers the screen, which is invisible and fail-safe, whereas
// shrinking it would show undimmed edges if the canvas assumption were off.
bool FixTagHelpPanel() {
    const auto& r = resolve::Get();
    if (!r.TagHelpDraw) { hoe::Log("taghelp: not resolved - panel left as-is"); return false; }

    struct Site { uintptr_t at; uintptr_t k; const char* what; };
    const Site sites[] = {
        { off::C_TAGHELP_SITE_ART_W,  off::C_CANVAS_W_CONST, "art width"  },
        { off::C_TAGHELP_SITE_SIZE_W, off::C_CANVAS_W_CONST, "size width" },
        { off::C_TAGHELP_SITE_POS_Y,  off::C_CANVAS_H_CONST, "centre y"   },
        { off::C_TAGHELP_SITE_POS_X,  off::C_CANVAS_W_CONST, "centre x"   },
    };

    static hoe::Patch patches[4];
    int done = 0;
    for (int i = 0; i < 4; ++i) {
        const uintptr_t rva = r.TagHelpDraw + sites[i].at;
        // Refuse unless the site really is the 17-byte getter sequence, so a
        // shifted function in a future build cannot be silently corrupted.
        auto p = (const unsigned char*)(hoe::Base() + rva);
        if (!(p[0] == 0xBA && p[1] == 0x02 && p[2] == 0 && p[3] == 0 && p[4] == 0 &&
              p[5] == 0x48 && p[6] == 0x8B && p[7] == 0x0D && p[12] == 0xE8)) {
            hoe::Log("taghelp: site %d (%s) at +0x%llX is not the expected sequence "
                     "- aborting, panel left as-is", i, sites[i].what,
                     (unsigned long long)rva);
            for (int k = 0; k < done; ++k) patches[k].Revert();
            return false;
        }
        unsigned char buf[17];
        buf[0] = 0xC5; buf[1] = 0xFA; buf[2] = 0x10; buf[3] = 0x05;
        const int disp = (int)((intptr_t)sites[i].k - (intptr_t)(rva + 8));
        memcpy(buf + 4, &disp, 4);
        memset(buf + 8, 0x90, 9);
        if (!patches[i].Apply(rva, buf, sizeof(buf), sites[i].what)) {
            for (int k = 0; k < done; ++k) patches[k].Revert();
            return false;
        }
        ++done;
    }
    hoe::Log("taghelp: Controls panel re-sized to the 1280x720 2D canvas "
             "(was sizing itself from the output resolution)");
    return true;
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

// Reports the layout's slot state.
//
// The resource table is reached through a POINTER at its RVA, not stored there:
// the engine's own access is `mov rax, qword ptr [rip+disp]` (a load) and boot
// writes a heap pointer into it. The first version of this function used the RVA
// as the table base, read unrelated static memory that happens to be zero, and
// duly reported an "empty resource entry" that did not exist. The neighbouring
// texture table really is in-place (`mov rcx,[r14+rsi*8+disp]`, r14 = image
// base), so those two are dereferenced differently on purpose.
bool LogLayoutResources(const char* when, void* layout) {
    if (!layout) { hoe::Log("  layoutres %s: layout is NULL", when); return false; }
    if (!s_pLayoutRes || !s_layoutTexPar) {
        hoe::Log("  layoutres %s: diagnostics did not resolve", when);
        return false;
    }
    auto table = (unsigned char*)*s_pLayoutRes;      // <- the deref that was missing
    if (!table) {
        hoe::Log("  layoutres %s: resource table pointer is NULL (layout system "
                 "not initialised yet)", when);
        return false;
    }

    auto l = (unsigned char*)layout;
    const int idx = *(int*)(l + off::C_LAYOUT_RES_INDEX_FIELD);
    // Slot -1 means "no free slot was available" and is a real engine outcome,
    // so it must be reported rather than indexed with.
    if (idx < 0 || idx >= (int)off::C_LAYOUT_SLOT_COUNT) {
        hoe::Log("  layoutres %s: slot %d outside the %d-slot tables%s",
                 when, idx, (int)off::C_LAYOUT_SLOT_COUNT,
                 idx == -1 ? " (-1 = the engine found no free slot)" : "");
        return false;
    }

    auto entry = table + (uintptr_t)idx * off::C_LAYOUT_RES_STRIDE;
    const unsigned resFlags = *(unsigned*)(entry + off::C_RES_FLAGS_FIELD);
    void* csb              = *(void**)(entry + off::C_RES_CSB_FIELD);
    const int pageCount    = *(int*)(entry + off::C_LAYOUT_RES_COUNT_FIELD);
    void* tex              = s_layoutTexPar[idx];
    const unsigned lFlags  = *(unsigned*)(l + off::C_LAYOUT_FLAGS_FIELD);
    auto name              = (const char*)(l + off::C_LAYOUT_NAME_FIELD);

    hoe::Log("  layoutres %s: slot=%d name='%.48s'", when, idx, name);
    hoe::Log("  layoutres %s: res.flags=0x%X (1=published 2=borrowed 4=awaiting.par "
             "8=csb-says-no-textures) csb=%p pages[+18]=%d texpar=%p",
             when, resFlags, csb, pageCount, tex);
    // Deliberately NOT calling sub_484510 here. It is a one-shot page BUILDER,
    // not a readiness test - on a layout whose bit0 is clear it would allocate
    // and populate the page vector as a side effect, using whatever `kind` we
    // guessed, silently changing element behaviour. Bit0 of layout.flags is the
    // same information for free, so a diagnostic reads that instead.
    hoe::Log("  layoutres %s: layout.flags=0x%X (bit0=pages built) haspages=%d",
             when, lFlags, LayoutHasPages ? LayoutHasPages(layout) : -1);

    if (resFlags & 8) {
        hoe::Log("  layoutres %s: the csb itself declares NO TEXTURES for this "
                 "layout - that alone would explain empty rendering", when);
    } else if (!tex) {
        hoe::Log("  layoutres %s: no texture archive in this slot while the csb "
                 "does not claim to be texture-free", when);
    }
    return true;
}

// Every live slot, side by side. A null texture handle on the recap's slot only
// means something if the layouts that ARE on screen have a non-null one, and
// this is the cheapest way to settle that without another play session.
void LogAllLayoutSlots(const char* when) {
    if (!s_pLayoutRes || !s_layoutTexPar) return;
    auto table = (unsigned char*)*s_pLayoutRes;
    if (!table) return;
    auto names = s_pLayoutNames ? (const char**)*s_pLayoutNames : nullptr;

    int live = 0, withTex = 0;
    hoe::Log("  slotdump %s: slot name                              "
             "res.flags pages texpar", when);
    for (int i = 0; i < (int)off::C_LAYOUT_SLOT_COUNT; ++i) {
        auto entry = table + (uintptr_t)i * off::C_LAYOUT_RES_STRIDE;
        const unsigned f = *(unsigned*)(entry + off::C_RES_FLAGS_FIELD);
        const int pages  = *(int*)(entry + off::C_LAYOUT_RES_COUNT_FIELD);
        void* tex        = s_layoutTexPar[i];
        if (!f && !pages && !tex) continue;            // slot never used
        ++live;
        if (tex) ++withTex;
        const char* nm = "?";
        if (names) {
            const char* n = names[i];
            // The table is engine-owned; only trust a plausible in-heap string.
            if (n && *n >= 0x20 && *n < 0x7F) nm = n;
        }
        hoe::Log("  slotdump %s: %4d %-32.32s 0x%-7X %5d %p",
                 when, i, nm, f, pages, tex);
    }
    hoe::Log("  slotdump %s: %d live slots, %d of them have a texture archive",
             when, live, withTex);
}

int LayoutSlot(void* layout) {
    if (!layout) return -1;
    const int idx = *(int*)((unsigned char*)layout + off::C_LAYOUT_RES_INDEX_FIELD);
    return (idx >= 0 && idx < (int)off::C_LAYOUT_SLOT_COUNT) ? idx : -1;
}

int LayoutPageCount(void* layout) {
    const int idx = LayoutSlot(layout);
    if (idx < 0 || !s_pLayoutRes) return -1;
    auto table = (unsigned char*)*s_pLayoutRes;
    if (!table) return -1;
    return *(int*)(table + (uintptr_t)idx * off::C_LAYOUT_RES_STRIDE
                         + off::C_LAYOUT_RES_COUNT_FIELD);
}

// MSVC stores a Complete Object Locator pointer immediately before every
// polymorphic vftable, so the class name is a couple of pointer hops away - no
// scanning. Worth it: it turns "vft=+0x1384968" in the log into a name we can
// act on straight away.
const char* MacroClassName(const void* obj) {
    const uintptr_t base = hoe::Base();
    const uintptr_t size = hoe::ImageSize();
    auto inImage = [&](uintptr_t rva) { return rva > 0 && rva + 32 < size; };
    if (!obj) return "?";

    const uintptr_t vft = *(const uintptr_t*)obj;
    if (vft < base || vft - base >= size) return "?";
    const uintptr_t col = *(const uintptr_t*)(vft - 8);
    if (col < base || col - base >= size) return "?";

    const uint32_t tdRva = *(const uint32_t*)(col + 12);
    if (!inImage(tdRva)) return "?";
    auto name = (const char*)(base + tdRva + 16);
    // Decorated names always start ".?AV"; anything else means we mis-stepped.
    return (name[0] == '.' && name[1] == '?') ? name : "?";
}

// A macro whose handler never queues a next step parks forever: `cur` stops
// changing and `next` sits at -1. That is precisely the House of Extras black
// screen, and printing it for whatever macro is running means any mode which
// hangs identifies itself, without needing a patch in that class first.
void PollMissionState() {
    if (!s_pMissionMgr) return;
    auto mgr = (unsigned char*)*s_pMissionMgr;
    if (!mgr) return;
    auto macro = *(unsigned char**)(mgr + off::C_MISSIONMGR_MACRO_FIELD);

    static void* s_lastMacro = nullptr;
    static int   s_lastCur = -2, s_lastNext = -2;
    static int   s_stuckTicks = 0;

    if (!macro) {
        if (s_lastMacro) {
            hoe::Log("mission: macro ended");
            s_lastMacro = nullptr; s_lastCur = s_lastNext = -2; s_stuckTicks = 0;
        }
        return;
    }

    const int id   = *(int*)(macro + off::C_MACRO_ID_FIELD);
    const int cur  = *(int*)(macro + off::C_CUR_STEP_FIELD);
    const int next = *(int*)(macro + off::C_NEXT_STEP_FIELD);
    const uintptr_t vft = *(uintptr_t*)macro - hoe::Base();

    if (macro != s_lastMacro) {
        hoe::Log("mission: macro %p id=0x%X %s (vft +0x%llX)", macro, id,
                 MacroClassName(macro), (unsigned long long)vft);
        s_lastMacro = macro; s_lastCur = s_lastNext = -2; s_stuckTicks = 0;
    }
    if (cur != s_lastCur || next != s_lastNext) {
        // Capped so a long session cannot balloon the log; the STUCK check
        // below keeps working after the cap, which is the part that matters.
        static int s_lines = 0;
        const int kMaxLines = 300;
        if (s_lines < kMaxLines) {
            hoe::Log("mission: step 0x%X -> next %d (id=0x%X)", cur, next, id);
        } else if (s_lines == kMaxLines) {
            hoe::Log("mission: step logging capped at %d lines; still watching "
                     "for a stuck step", kMaxLines);
        }
        ++s_lines;
        s_lastCur = cur; s_lastNext = next; s_stuckTicks = 0;
        return;
    }

    // Parked: same step, nothing queued. This is the black-screen signature,
    // but it is NOT proof on its own - plenty of macros idle this way while
    // waiting on input (the title screen sits at step 0x3B with next -1). So
    // wait a good while and word it as an observation, not a verdict.
    const int kParkedTicks = 15;                 // ~30s at the 2s heartbeat
    if (next == -1 && ++s_stuckTicks == kParkedTicks) {
        hoe::Log("mission: parked ~%ds at step 0x%X with nothing queued - %s "
                 "(id=0x%X). Normal while a macro waits on input; if the screen "
                 "is black this is the signature of a missing step handler.",
                 kParkedTicks * 2, cur, MacroClassName(macro), id);
    }
}

// --- replay Bob's post-match reset ------------------------------------------
//
// WHY THIS EXISTS. Starting a House of Extras mode is EDGE-TRIGGERED on a save
// flag, not a command. Bob's menu row is a talk-script op 0x0105 whose entire
// action is `sub_BF8B30(flagMgr, group, bit, 1)`. That function does not write
// the flag - it appends to a pending-changes list, and it does ABSOLUTELY
// NOTHING when the committed bank already holds the requested value. With no
// pending entry, sub_BF87D0 returns -1, sub_CBBE20 bails on -1, and
// sub_B46630 (StartMission) is never reached. The row still highlights, the
// dialogue still plays, and nothing happens - silently, per mode, forever.
//
// So after a run the mode's own "start" bit is stuck at 1 and that row is dead.
// The retail game clears it from Bob's OWN script (uid00ee05b6.msg @0x79CC),
// but that block is reached only when エクストラ_終了 (332:29) or 中断 (332:28)
// is set, and on a normal PC completion neither ever is: the only setters are
// CMissionMacroAdventureSurvive slot 102 and the Adventure* slot 11 (gated on
// 199:10), and ColosseumExtra's slot-10 teardown sets 中断 on the ABORT branch
// only. So we replay that block ourselves, flag for flag, in its own order.
//
// Note this is NOT what CommitFlags does. CommitFlags only APPLIES pending
// changes - it cannot clear a bank bit that is already 1 - which is exactly why
// an earlier attempt with it measured as "ran, symptom unchanged".
namespace {

struct ExtraFlag { short group; short bit; const char* name; };

// ONLY the flags that are actually on the dispatch path. This list used to be
// 28 flags copied from Bob's script; that was wrong and it made things worse.
//
// What the scenario data actually says (decoded from data/scenario_en/scenario.bin,
// the real flag->mission trigger table - it is DATA, not code):
//   199:2..9  section 0x601 "network" entries e1..e8 -> mission id 414. LIVE
//             TRIGGERS, and id 414 exists on PC and constructs fine. But unlike
//             e9 (Extra Chase, 199:10) which clears its own trigger with
//             `OP19 199 10 0`, e1..e8 NEVER CLEAR THEIRS. That single data
//             omission is why an extras mode can be started exactly once per
//             save: the bit stays 1, so Bob's sub_BF8B30(199,b,1) sees the bank
//             already at 1 and returns WITHOUT queuing anything (+0xBF8C22).
//   1:36      -> mission 143, the deleted ranking macro. Harmless in itself
//             (Request(143) self-cancels at +0xB6AFCB because there is no
//             mission record for it), but sub_BF87D0 returns the FIRST pending
//             match and removes nothing, so a stale {1,36,1} sitting ahead of
//             {199,b,1} STARVES the real trigger. Clear it.
//
// Deliberately NOT cleared:
//   199:0  not a trigger at all - zero condition-program references in 752
//          entries; it is a "network mode active" marker the missions SET.
//          Leaving it at 1 is strictly better: Bob's row calls BF8B30(199,0,1)
//          first, which then no-ops, so the pending list ends up holding only
//          the entry that matters.
//   332:12..27  macro-local mode/character selectors that mission 414 itself
//          reads and its destructor sub_B57F40 clears (FlagSetRaw(...,0) at
//          +0xB58011 and seven sibling sites through +0xB5831C). They self-heal.
//
// CORRECTION, and it cost the tag modes: this block used to say group 332 has
// "zero condition references anywhere". That is true of 332:12..27 and FALSE of
// 332:6, which IS a live trigger - scenario.bin section 0x502 entry 0, condition
// OP16(332,6,1) -> mission 304, the OFFLINE Survival Tag row. Bob's menu has TWO
// tag rows (offline 332:6, ranking 199:1) that both start mission 304, and
// nothing anywhere clears 332:6 - not the scenario body, not the macro
// destructor, not the executable. Bob's own retail reset block clears it
// (uid00ee05b6.msg +0x7A1C), so we must too.
const ExtraFlag kExtrasReset[] = {
    { 199, 1, "NR_onigokko" },
    { 199, 2, "NR_BattleKing_Akiyama" },  { 199, 3, "NR_BattleKing_Saejima" },
    { 199, 4, "NR_BattleKing_Tanimura" }, { 199, 5, "NR_BattleKing_Kiryu" },
    { 199, 6, "NR_FastestKiller_Akiyama" }, { 199, 7, "NR_FastestKiller_Saejima" },
    { 199, 8, "NR_FastestKiller_Tanimura" },{ 199, 9, "NR_FastestKiller_Kiryu" },
    {   1, 36, "COMMON_ranking_display" },
    { 332,  6, "NR_SurvivalTag_offline" },
    // Speed King. Its scenario entry (section 0x0601 e09, mission 301) does carry
    // an OP19 that clears 199:10 - but OP19 is the mission-COMPLETE op list, and
    // that entry has no OP18 (abort) list at all. So a lost or abandoned Extra
    // Chase never clears its own trigger and the row stays dead. Clearing it here
    // is a no-op whenever the bit already reads 0.
    { 199, 10, "NR_ExtraChase_SpeedKing" },
    // Speed King INTRO (title card + jingle + 3-2-1). 332:10 is "caption seen":
    // caption.bin entry 130 (2d_mn_ch_dlc_bakuso_ch, id 102) is current only
    // while 199:10==1 AND 332:10==0, and its completion op (OP11) sets 332:10=1
    // when the card ends. Identical data on PS3 and PC. The only clearer is Bob's
    // retail reset block (uid00ee05b6.msg +0x7A04 on PC, +0x6F58 on PS3), which
    // PC never reaches - so run 2 of a process had the black window but no card
    // (caption-trace: cursub=-1 page=-1 on run 1). Mirror that row here, gated so
    // it can be switched off (ChaseResetCaptionSeen) rather than deleted.
    { 332, 10, "NR_ExtraChase_SpeedKing_caption_seen" },
};

// Rows of kExtrasReset that a config key can switch off. Everything else is
// unconditional.
bool ResetRowEnabled(const ExtraFlag& f) {
    if (f.group == 332 && f.bit == 10) return config::Get().ChaseResetCaptionSeen != 0;
    return true;
}

}  // namespace

bool ResetExtrasStartFlags(bool logOnly) {
    if (!pFlagMgr) return false;
    void* mgr = *pFlagMgr;
    if (!mgr) { hoe::Log("extras-reset: no flag manager"); return false; }
    if (!FlagGet) { hoe::Log("extras-reset: FlagGet unresolved"); return false; }
    if (!logOnly && (!FlagSetRaw || !FlagSetEdge)) {
        hoe::Log("extras-reset: flag setters unresolved - logging only");
        logOnly = true;
    }

    int wereSet = 0;
    char line[512];
    int len = 0;
    for (const auto& f : kExtrasReset) {
        if (!ResetRowEnabled(f)) continue;
        const int before = FlagGet(mgr, f.group, f.bit, 1);
        if (before) {
            ++wereSet;
            if (len < (int)sizeof(line) - 40) {
                len += snprintf(line + len, sizeof(line) - len, "%s%d:%d",
                                len ? " " : "", f.group, f.bit);
            }
        }
        if (logOnly || !before) continue;

        // ORDER IS LOAD-BEARING: bank FIRST, then the pending list.
        //
        // The reverse order is a trap, and it cost a whole test run. Calling
        // FlagSetEdge(...,0) while the bank still reads 1 finds no entry, sees
        // cur^value == 1, and APPENDS a pending {g,b,0}. A later
        // FlagSetEdge(...,0) then finds that entry with a matching value and
        // returns WITHOUT removing it. The stale zero-entry survives - and the
        // next time Bob's row calls FlagSetEdge(g,b,1) it finds it, treats the
        // mismatch as "cancel the pending change", removes it, and never queues
        // the start. The player's press is silently eaten. Verified live: the
        // pending list held {1,36,0} and the 199 entries had been consumed by
        // exactly one failed attempt.
        //
        // Bank first, list second: FlagSetRaw does not touch the list, so the
        // following FlagSetEdge sees cur == 0 == value and does nothing at all
        // (and still removes a stale {g,b,1} if one is queued). List stays clean.
        FlagSetRaw (mgr, f.group, f.bit, 0);
        FlagSetEdge(mgr, f.group, f.bit, 0);
    }

    if (!wereSet) {
        hoe::Log("extras-reset: nothing was set - no start flag is stuck");
        return true;
    }
    hoe::Log("extras-reset: %d flag(s) were set [%s]%s", wereSet, line,
             logOnly ? " (log only, not cleared)" : " - cleared");

    if (logOnly) return true;

    int stillSet = 0;
    for (const auto& f : kExtrasReset) {
        if (!ResetRowEnabled(f)) continue;
        if (FlagGet(mgr, f.group, f.bit, 1)) {
            ++stillSet;
            hoe::Log("extras-reset: WARNING %d:%d (%s) is STILL set after clearing",
                     f.group, f.bit, f.name);
        }
    }
    if (!stillSet) hoe::Log("extras-reset: verified - every flag now reads 0");

    // The bank is only half the state. A leftover entry here is what swallowed
    // the player's next button press, so say out loud what the list holds.
    auto fm = (unsigned char*)mgr;
    const unsigned count = *(unsigned*)fm;
    if (count == 0) {
        hoe::Log("extras-reset: pending-change list is empty - clean");
    } else if (count <= 32) {
        char pend[256]; int pl = 0;
        for (unsigned i = 0; i < count && pl < (int)sizeof(pend) - 24; ++i) {
            auto e = (unsigned*)(fm + 4 + i * 12);
            pl += snprintf(pend + pl, sizeof(pend) - pl, "%s%u:%u=%u",
                           pl ? " " : "", e[0], e[1], e[2]);
        }
        hoe::Log("extras-reset: pending-change list holds %u entry(s) [%s] - "
                 "any of ours here would eat the next attempt", count, pend);
    } else {
        hoe::Log("extras-reset: pending-change list count reads %u (>32) - "
                 "not trusting that, skipping the dump", count);
    }
    return stillSet == 0;
}

// --- the arm/fire latch -----------------------------------------------------
//
// The reset must happen AFTER the ColosseumExtra macro is destroyed. Several of
// the mode's own functions read these flags to identify which mode is running,
// so clearing while it is still alive would break the run in progress. There is
// no game-thread hook after the recap ends, so the background heartbeat fires
// it - the same thread that already re-applies the mode unlock.
namespace {
volatile long s_resetArmed = 0;
int s_macroGoneStreak = 0;
// The macro that was running when the recap ended. The reset must not touch the
// extras flags until this one is gone - 199:2..5 are not only triggers, they are
// what the extras code reads to know WHICH character and mode is running, and
// the macro is still alive through step 0x2E and its destructor.
void* s_extrasMacro = nullptr;
}

// Watch the live macro slot and fire the reset when a MODE macro is replaced.
//
// This exists so that no mode needs a hook. Two things defeated the old
// hook-based arming: the tag modes run under a different macro class entirely,
// and even ColosseumExtra can end without passing through our recap steps - a
// lost run, which the user hit and the log confirms (a ColosseumExtra macro at
// 02:54:27 with no step 0x2C and no reset following it).
//
// SAFETY, and it is the whole design. The bit cleared here is the same bit Bob
// SETS to start a mode, so firing at the wrong moment silently eats the player's
// press. Hence: fire only when a macro we LATCHED has been replaced by something
// that is not itself a mode macro.
//   palace -> mode   nothing was latched, so no fire. The new bit survives.
//   mode   -> palace latched, replaced by a non-mode macro: FIRE. This is the
//                    only case, and it is unambiguously "the run is over".
//   mode   -> mode   deliberately does NOT fire: the incoming mode's freshly set
//                    bit must survive. In practice the palace always intervenes.
// The latched pointer is only ever compared, never dereferenced after the swap -
// by then it points at freed memory.
namespace { int s_modeEndCount = 0; }
int ModeEndCount() { return s_modeEndCount; }

// --- Bob's post-mode line --------------------------------------------------
//
// Bob's stage pac has three entries for the moment the player comes back from
// a mode, gated on EXTRA_abort (332:28) and EXTRA_end (332:29): both set ->
// "What, done already?", 332:29 alone -> "Well? Did you enjoy yourself?". On
// PS3 the mode code sets them (AdventureSurvive slot 102 and the Adventure
// slot-11 teardown set 332:29; ColosseumExtra's abort branch sets 332:28). PC
// kept the pac but lost every setter with the ranking code: the mode-end gate
// log read 332:28=0 332:29=0 166:0=0 after a finished run AND after a quit.
// So the mode-end handler writes them itself. Bank first, then the pending
// list - the same order rule as the reset above.
namespace {
volatile long s_runResult = 0;
}

void NoteRunResult(const char* who) {
    if (InterlockedExchange(&s_runResult, 1) == 0)
        hoe::Log("post-mode: run produced a result (%s) - Bob gets the 'enjoy' line", who);
}

namespace {
void SetBobPostModeFlags(bool completed) {
    void* fm = pFlagMgr ? *pFlagMgr : nullptr;
    if (!fm || !FlagSetRaw || !FlagSetEdge) {
        hoe::Log("post-mode: flag manager or setters unavailable - Bob's flags not set");
        return;
    }
    FlagSetRaw(fm, 332, 29, 1);
    FlagSetEdge(fm, 332, 29, 1);
    if (!completed) {
        FlagSetRaw(fm, 332, 28, 1);
        FlagSetEdge(fm, 332, 28, 1);
    }
    hoe::Log("post-mode: set 332:29%s for Bob (%s)",
             completed ? "" : " + 332:28", completed ? "finished run" : "no result - quit");
}
}  // namespace

void ObserveModeEnd() {
    if (!config::Get().ResetExtrasFlags) return;
    if (!s_pMissionMgr) return;
    auto mgr = (unsigned char*)*s_pMissionMgr;
    if (!mgr) return;

    const auto& r = resolve::Get();
    void* cur = *(void**)(mgr + off::C_MISSIONMGR_CURMACRO);

    auto isMode = [&](void* m) -> bool {
        if (!m) return false;
        const uintptr_t vt = (uintptr_t)*(void**)m - hoe::Base();
        return (r.ColosExtraVft  && vt == r.ColosExtraVft)
            || (r.AdvSurviveVft  && vt == r.AdvSurviveVft)
            || (r.AdvEscapeVft   && vt == r.AdvEscapeVft);
    };

    // AdventureEscape needs a second condition that the other two do not.
    //
    // Its mission 301 is SHARED with seven story chases, so recognising the class
    // alone would make every story chase in the game clear the extras start
    // flags - and the one moment that actually hurts is a player who has just
    // armed a row at Bob and has not started it yet. Flag 199:10 is what Bob sets
    // to launch Speed King and is the scenario's own trigger condition for that
    // entry, so it is set for the whole ranked run and clear for a story chase.
    //
    // It is read at LATCH time, mid-run, rather than at reset time - by the time
    // the macro has been replaced the flag may already be gone.
    // Logged once per chase macro, so a story chase leaves POSITIVE evidence that
    // the gate held rather than requiring anyone to confirm an absence. Once per
    // macro and not per frame: this runs from the 2D draw hook, so an unguarded
    // log here would emit sixty lines a second for the length of a chase.
    static void* s_judgedChase = nullptr;

    auto latchable = [&](void* m) -> bool {
        if (!isMode(m)) return false;
        const uintptr_t vt = (uintptr_t)*(void**)m - hoe::Base();
        if (!(r.AdvEscapeVft && vt == r.AdvEscapeVft)) return true;

        void* fm = pFlagMgr ? *pFlagMgr : nullptr;
        const bool ready = fm && FlagGet && *(void**)((unsigned char*)fm + off::C_FLAGMGR_TABLE_FIELD);
        const int f = ready ? FlagGet(fm, 199, 10, 1) : -1;
        const bool mine = (f == 1);
        if (m != s_judgedChase) {
            s_judgedChase = m;
            hoe::Log("mode-end: AdventureEscape chase (macro %p) 199:10=%d -> %s",
                     m, f,
                     mine ? "Speed King - will watch once its clock stops"
                          : "STORY chase - ignoring, no extras flags will be touched");
        }
        // Not until the run's clock has STOPPED. A chase swaps macros mid-run -
        // the title card and countdown run under CMissionMacroAdventure step 0xC
        // and the Escape macro comes back afterwards - and latching on first
        // sight made that handoff look like the mode ending: the reset fired
        // five seconds into a run (logged 11:26:20 on a run that ended 11:26:54).
        // Because cur != s_latched while this returns false, it is re-evaluated
        // every frame and latches on the frame the timer stops.
        return mine && ChaseRunFinished();
    };

    static void* s_latched = nullptr;

    if (cur == s_latched) return;          // nothing changed
    if (s_latched && !isMode(cur)) {
        s_latched = nullptr;
        ++s_modeEndCount;
        // Say what took its place: NULL and a palace macro are the expected
        // shapes; anything else at this point is worth knowing about.
        const uintptr_t nvt = cur ? (uintptr_t)*(void**)cur - hoe::Base() : 0;
        hoe::Log("mode-end: a mode macro was replaced (now %p, vft +0x%llX) - "
                 "clearing start flags", cur, (unsigned long long)nvt);
        ResetExtrasStartFlags(config::Get().DiagFlagsOnly != 0);
        // Bob's post-mode gates. The run-result note is consumed here whether or
        // not the write is enabled, so a stale note never leaks into the next run.
        const bool completed = InterlockedExchange(&s_runResult, 0) != 0;
        if (config::Get().PostModeBobFlags) SetBobPostModeFlags(completed);
        // Read AFTER the write so the log shows what his talk entries will see:
        // 332:28 (abort) & 332:29 (end) -> "done already", 166:0 & 332:29 /
        // 332:29 alone -> "enjoy yourself". Not in the reset list; PS3's Bob
        // script is what clears them.
        if (pFlagMgr && *pFlagMgr && FlagGet) {
            void* fm = *pFlagMgr;
            hoe::Log("mode-end: Bob gates 332:28=%d 332:29=%d 166:0=%d",
                     FlagGet(fm, 332, 28, 1), FlagGet(fm, 332, 29, 1), FlagGet(fm, 166, 0, 1));
        }
        return;
    }
    s_latched = latchable(cur) ? cur : nullptr;
}

void ArmExtrasReset(void* extrasMacro) {
    s_extrasMacro = extrasMacro;
    InterlockedExchange(&s_resetArmed, 1);
    s_macroGoneStreak = 0;
    if (config::Get().DiagMissionTrace) StartMissionTrace();
}

void PollExtrasReset() {
    if (InterlockedCompareExchange(&s_resetArmed, 1, 1) != 1) return;
    if (!config::Get().ResetExtrasFlags) { InterlockedExchange(&s_resetArmed, 0); return; }

    // Require the macro slot to read empty on two consecutive polls before
    // touching anything - one sample could catch a gap between macros.
    if (!s_pMissionMgr) return;
    auto mgr = (unsigned char*)*s_pMissionMgr;
    if (!mgr) return;
    // +0x50, NOT +0x58. The engine reads the live macro from +0x50 (see
    // +0xB6AFD0, which calls slot 4 on it before swapping); +0x58 holds the
    // INCOMING macro only for the instant between +0xB6AFF2 storing it and
    // +0xB6AD5E zeroing it, so gating on it passed unconditionally.
    if (*(void**)(mgr + off::C_MISSIONMGR_CURMACRO)) { s_macroGoneStreak = 0; return; }
    if (++s_macroGoneStreak < 2) return;

    if (InterlockedCompareExchange(&s_resetArmed, 0, 1) != 1) return;
    s_macroGoneStreak = 0;
    ResetExtrasStartFlags(config::Get().DiagFlagsOnly != 0);
}


// --- the mission-manager trace ----------------------------------------------
//
// One run of this discriminates every surviving hypothesis about why a played
// mode will not restart, and why Bob then goes dead. Samples five values fast
// enough to catch the moment the player confirms Bob's row - the 2s unlock
// heartbeat is far too coarse for that.
//
//   A  GetFlag(199, b, 1)      the trigger bit in the committed bank
//   B  pending-list count + entries at g_flagMgr[0]
//   C  missionMgr->[0x30]      manager state; 0 idle, 3 is the stall state
//   D  missionMgr->[0x2C]      requested mission id
//   E  curMacro->[0x18]        outgoing macro lifecycle; 3 == running
//
// Reading it:
//   A==1 and C stays 0 and B never gains {199,b,1}  -> the row is a provable
//        no-op at +0xBF8C22; the flag clear is the whole fix.
//   A==0, B gains the entry, C goes 1->2->3 and STICKS at 3 -> state-3 stall:
//        the outgoing macro never answers its readiness vfunc, which would
//        explain BOTH "nothing happens" and "Bob goes dead". Fix would belong
//        in the teardown, not the flag layer.
//   C stays 0 but E != 3 -> the request was refused by the guard at +0xB6B25B,
//        the same condition that kills every NPC gate. Also a teardown fault.
//   B shows a {199,b,0} entry -> a clear ran in the wrong order and poisoned
//        the list (bank must be written BEFORE the pending list is scrubbed).
int s_idleStreak = 0;

// Fire the reset now, without the heartbeat's macro-slot streak. Called from the
// 250ms sampler once the mission manager reports idle.
void PollExtrasResetNow() {
    // Atomically claim the arm flag. The previous test-then-clear let the 250ms
    // sampler and the 2s heartbeat both pass the check and both run the reset -
    // harmless because it is idempotent, but it doubled every log line.
    if (InterlockedCompareExchange(&s_resetArmed, 0, 1) != 1) return;
    if (!config::Get().ResetExtrasFlags) return;
    s_idleStreak = 0;
    s_extrasMacro = nullptr;
    ResetExtrasStartFlags(config::Get().DiagFlagsOnly != 0);
}

static DWORD WINAPI MissionTraceThread(LPVOID) {
    const int kSamples = 480;          // 480 * 250ms = 2 minutes
    unsigned lastSig = 0xFFFFFFFFu;
    int quiet = 0;

    for (int i = 0; i < kSamples; ++i) {
        Sleep(250);
        void* fm = pFlagMgr ? *pFlagMgr : nullptr;
        if (!fm || !s_pMissionMgr) continue;
        auto mgr = (unsigned char*)*s_pMissionMgr;
        if (!mgr) continue;

        const unsigned state = *(unsigned*)(mgr + off::C_MISSIONMGR_STATE);
        const unsigned req   = *(unsigned*)(mgr + off::C_MISSIONMGR_REQ_ID);
        auto cur = *(unsigned char**)(mgr + off::C_MISSIONMGR_CURMACRO);
        const int life = cur ? *(int*)(cur + off::C_MACRO_LIFECYCLE) : -1;
        const unsigned pend = *(unsigned*)fm;
        const int a = FlagGet ? FlagGet(fm, 199, 5, 1) : -1;

        // Fire on the actual EVENT: the extras macro has been replaced by
        // another one (in practice CMissionMacroAdventure taking the palace
        // back). That is a real state transition, so there is no debounce and
        // no arbitrary sample count - the previous version waited for "manager
        // idle for two consecutive samples", which was a proxy for this and
        // fired later than it needed to.
        //
        // We cannot simply clear at Finish(): the extras macro is still alive
        // there, and 199:2..5 are what its own code reads to know which
        // character and mode is running. Once a DIFFERENT macro owns the slot,
        // nothing can be reading them any more.
        auto curMacro = *(void**)(mgr + off::C_MISSIONMGR_MACRO_FIELD);
        if (curMacro && curMacro != s_extrasMacro) {
            PollExtrasResetNow();
        } else if (!curMacro && ++s_idleStreak >= 8) {
            // Backstop: no macro took over within ~2s. Firing on an empty slot
            // is still safe - the extras macro is demonstrably gone - and this
            // stops a quiet handover from stranding the flags set forever.
            PollExtrasResetNow();
        } else if (curMacro) {
            s_idleStreak = 0;
        }

        // Only speak when something changes; otherwise this floods the log.
        const unsigned sig = (state << 24) ^ (req << 12) ^ ((unsigned)(life + 2) << 8) ^ pend
                           ^ ((unsigned)(a + 1) << 4);
        if (sig == lastSig) { if (++quiet < 40) continue; }
        quiet = 0;
        lastSig = sig;

        // MUST be terminated up front: when the list is empty the loop below
        // never runs, and an unterminated buffer prints stale stack from the
        // previous sample - which produced impossible "pending=0[1:36=1]" lines.
        char pd[192]; pd[0] = 0; int pl = 0;
        if (pend && pend <= 32) {
            for (unsigned k = 0; k < pend && pl < (int)sizeof(pd) - 24; ++k) {
                auto e = (unsigned*)((unsigned char*)fm + 4 + k * 12);
                pl += snprintf(pd + pl, sizeof(pd) - pl, "%s%u:%u=%u",
                               pl ? "," : "", e[0], e[1], e[2]);
            }
        }
        hoe::Log("trace: flag199:5=%d pending=%u[%s] state=%u reqId=%u macroLife=%d",
                 a, pend, pd, state, req, life);
    }
    hoe::Log("trace: window closed");
    return 0;
}

void StartMissionTrace() {
    static volatile long s_running = 0;
    if (InterlockedCompareExchange(&s_running, 1, 0) != 0) return;
    if (HANDLE h = CreateThread(nullptr, 0, MissionTraceThread, nullptr, 0, nullptr)) {
        CloseHandle(h);
        hoe::Log("trace: watching the mission manager for 2 minutes");
    }
    InterlockedExchange(&s_running, 0);
}

}  // namespace game
