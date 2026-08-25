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
LayoutIsLoadingFn LayoutIsLoading = nullptr;
LayoutPageReadyFn LayoutPageReady = nullptr;
static unsigned char* s_layoutRes    = nullptr;
static void**         s_layoutTexPar = nullptr;
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
        pDlcMask     = (uint32_t*)     (b + r.GDlcMask);
        if (r.IsDlcOwned) IsDlcOwned = (IsDlcOwnedFn)(b + r.IsDlcOwned);
    }
    if (r.missionDiagOk) s_pMissionMgr = (void**)(b + r.GMissionMgr);
    if (r.layoutDiagOk) {
        LayoutIsLoading = (LayoutIsLoadingFn)(b + r.LayoutIsLoading);
        LayoutPageReady = (LayoutPageReadyFn)(b + r.LayoutPageReady);
        s_layoutRes     = (unsigned char*)   (b + r.GLayoutRes);
        s_layoutTexPar  = (void**)           (b + r.GLayoutTexPar);
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

// A layout carries an index into a global resource table (stride 0x70), whose
// +0x18 field is how many elements the engine registered for it. The matching
// slot in a *second* table holds the layout's texture archive - loaded by a
// separate path that LoadLayout never invokes. A non-zero element count with a
// null texture handle is the signature of a layout that runs perfectly and
// draws nothing, which is exactly what screen 220 does.
bool LogLayoutResources(const char* when, void* layout) {
    if (!layout) { hoe::Log("  layoutres %s: layout is NULL", when); return false; }
    if (!s_layoutRes || !s_layoutTexPar) {
        hoe::Log("  layoutres %s: diagnostics did not resolve", when);
        return false;
    }
    auto l = (unsigned char*)layout;
    const int idx = *(int*)(l + off::C_LAYOUT_RES_INDEX_FIELD);
    // Layout slots are few: three parallel per-slot tables sit back to back,
    // and the tightest gap (the dword table at 0x19809E0 ending exactly where
    // the texture table begins) puts the capacity at 40. An unbounded index
    // here would read well past the table into unrelated globals.
    if (idx < 0 || idx >= (int)off::C_LAYOUT_SLOT_COUNT) {
        hoe::Log("  layoutres %s: resource index %d outside the %d-slot tables",
                 when, idx, (int)off::C_LAYOUT_SLOT_COUNT);
        return false;
    }
    auto entry = s_layoutRes + (uintptr_t)idx * off::C_LAYOUT_RES_STRIDE;
    const int elems = *(int*)(entry + off::C_LAYOUT_RES_COUNT_FIELD);
    void* tex = s_layoutTexPar[idx];
    const int loading = LayoutIsLoading ? LayoutIsLoading(layout) : -1;
    const int ready   = LayoutPageReady ? LayoutPageReady(layout, 0) : -1;

    hoe::Log("  layoutres %s: idx=%d elems[+18]=%d texpar=%p loading=%d page0ready=%d",
             when, idx, elems, tex, loading, ready);

    // The field meanings come from one caller (the network-ranking binder), so
    // rather than assert a conclusion we cannot yet back, dump the raw entry.
    // Whatever the fields turn out to mean, the evidence is in the log.
    char hex[3 * 32 + 1];
    for (int i = 0; i < 32; ++i) {
        static const char* d = "0123456789ABCDEF";
        hex[i * 3] = d[entry[i] >> 4];
        hex[i * 3 + 1] = d[entry[i] & 0xF];
        hex[i * 3 + 2] = (i == 15) ? '|' : ' ';
    }
    hex[3 * 32] = 0;
    hoe::Log("  layoutres %s: entry %s", when, hex);
    if (!tex) {
        hoe::Log("  layoutres %s: HYPOTHESIS - no texture archive in this layout's "
                 "slot. LoadLayout only reads <name>.csb; textures come from a "
                 "separate loader. That would explain a live, animating, "
                 "draw-gated layout rendering nothing.", when);
    }
    return true;
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

}  // namespace game
