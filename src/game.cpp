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

}  // namespace game
