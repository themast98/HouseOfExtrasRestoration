#include "resolve.h"
#include "offsets.h"
#include "patch.h"
#include "log.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <initializer_list>

namespace resolve {
namespace {
uintptr_t FindPattern(const char* pat);

const unsigned char* Img() {
    return reinterpret_cast<const unsigned char*>(hoe::Base());
}

bool InImage(uintptr_t rva) {
    return rva > 0 && rva < hoe::ImageSize();
}

// Follow incremental-link thunks (E9 rel32), max 4 hops.
uintptr_t Thunk(uintptr_t rva) {
    const unsigned char* img = Img();
    for (int i = 0; i < 4; ++i) {
        if (!InImage(rva) || img[rva] != 0xE9) break;
        int32_t rel;
        memcpy(&rel, img + rva + 1, 4);
        rva = rva + 5 + rel;
    }
    return rva;
}

// Target of a `call rel32` at rva.
uintptr_t ReadCall(uintptr_t rva) {
    const unsigned char* img = Img();
    if (!InImage(rva) || img[rva] != 0xE8) return 0;
    int32_t rel;
    memcpy(&rel, img + rva + 1, 4);
    return Thunk(rva + 5 + rel);
}

// Raw disp32 read. Some tables are reached through an absolute displacement
// with a zeroed base register rather than a RIP-relative operand, so the
// displacement IS the RVA and must not have the instruction length added.
uint32_t ReadU32At(uintptr_t rva) {
    const unsigned char* img = Img();
    if (!InImage(rva)) return 0;
    uint32_t v;
    memcpy(&v, img + rva, 4);
    return v;
}

// RIP-relative operand whose disp32 occupies the last 4 bytes of the instruction.
uintptr_t ReadRipRef(uintptr_t rva, uintptr_t len) {
    const unsigned char* img = Img();
    if (!InImage(rva)) return 0;
    int32_t disp;
    memcpy(&disp, img + rva + len - 4, 4);
    return rva + len + disp;
}

// Scan .text for a "AA BB ?? CC" style pattern. Returns RVA or 0.
uintptr_t FindPattern(const char* pat) {
    unsigned char bytes[64];
    bool wild[64];
    int n = 0;
    for (const char* p = pat; *p && n < 64;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { wild[n] = true; bytes[n] = 0; ++n; while (*p && *p != ' ') ++p; continue; }
        unsigned v = 0;
        int got = sscanf(p, "%2x", &v);
        if (got != 1) break;
        wild[n] = false; bytes[n] = (unsigned char)v; ++n;
        while (*p && *p != ' ') ++p;
    }
    if (n == 0) return 0;

    const unsigned char* img = Img();
    const uintptr_t lo = 0x1000, hi = hoe::ImageSize() - n;
    uintptr_t found = 0;
    int hits = 0;
    for (uintptr_t i = lo; i < hi; ++i) {
        int k = 0;
        for (; k < n; ++k) {
            if (!wild[k] && img[i + k] != bytes[k]) break;
        }
        if (k == n) {
            if (++hits == 1) found = i;
            if (hits > 1) {
                hoe::Log("  pattern is AMBIGUOUS (%d hits) - refusing", hits);
                return 0;
            }
        }
    }
    return found;
}

// RTTI: type descriptor -> complete object locator -> vftable RVA.
uintptr_t FindVTable(const char* typeName) {
    const unsigned char* img = Img();
    const size_t size = hoe::ImageSize();
    const size_t tn = strlen(typeName);

    // locate the (unique) type-descriptor name
    uintptr_t strRva = 0;
    int hits = 0;
    for (uintptr_t i = 0x1000; i + tn + 1 < size; ++i) {
        if (img[i] == '.' && memcmp(img + i, typeName, tn + 1) == 0) {
            if (++hits == 1) strRva = i;
        }
    }
    if (hits != 1) {
        hoe::Log("  RTTI name '%s' found %d times - expected exactly 1", typeName, hits);
        return 0;
    }
    const uint32_t td = (uint32_t)(strRva - 16);

    // find the complete object locator that points at it
    const uintptr_t imgBase = hoe::Base();
    for (uintptr_t off = 0; off + 24 < size; off += 4) {
        uint32_t sig, pTd, pSelf;
        memcpy(&sig,   img + off,      4);
        if (sig != 1) continue;
        memcpy(&pTd,   img + off + 12, 4);
        if (pTd != td) continue;
        memcpy(&pSelf, img + off + 20, 4);
        if (pSelf != (uint32_t)off) continue;

        // a qword pointing at the COL sits immediately before the vftable
        const uint64_t want = (uint64_t)(imgBase + off);
        for (uintptr_t j = 0; j + 8 < size; j += 8) {
            uint64_t q;
            memcpy(&q, img + j, 8);
            if (q == want) return j + 8;
        }
    }
    hoe::Log("  no vftable found for '%s'", typeName);
    return 0;
}

uintptr_t Slot(uintptr_t vt, int index) {
    if (!vt) return 0;
    uint64_t q;
    memcpy(&q, Img() + vt + index * 8, 8);
    uintptr_t rva = (uintptr_t)(q - hoe::Base());
    return InImage(rva) ? Thunk(rva) : 0;
}

void Check(const char* name, uintptr_t got, uintptr_t expect) {
    if (got == expect) {
        hoe::Log("  %-18s +0x%-9llX ok", name, (unsigned long long)got);
    } else {
        hoe::Log("  %-18s +0x%-9llX MISMATCH (expected +0x%llX for the 2024-07-21 build)",
                 name, (unsigned long long)got, (unsigned long long)expect);
    }
}

Resolved Build() {
    Resolved r;
    hoe::Log("resolving addresses (RTTI + anchor, no hardcoded RVAs)...");

    // --- RTTI walk: the two patch sites and the two reference handlers ---
    r.ActionExtraVft = FindVTable(off::TYPENAME_ACTION_COLOSSEUM_EXTRA);
    hoe::Log("  CActionColosseumExtra vft +0x%llX", (unsigned long long)r.ActionExtraVft);

    // The tag modes run under their own macro, so the flag reset has to
    // recognise it too - see game.cpp ObserveModeEnd.
    r.AdvSurviveVft = FindVTable(off::TYPENAME_ADVENTURE_SURVIVE);
    r.AdvEscapeVft  = FindVTable(off::TYPENAME_ADVENTURE_ESCAPE);
    hoe::Log("  CMissionMacroAdventureEscape vft +0x%llX",
             (unsigned long long)r.AdvEscapeVft);
    hoe::Log("  CMissionMacroAdventureSurvive vft +0x%llX",
             (unsigned long long)r.AdvSurviveVft);
    uintptr_t vtExtra = FindVTable(off::TYPENAME_COLOSSEUM_EXTRA);
    uintptr_t vtColos = FindVTable(off::TYPENAME_COLOSSEUM);
    if (!vtExtra || !vtColos) { hoe::Log("RTTI walk failed"); return r; }
    r.ColosExtraVft = vtExtra;   // kept for ObserveModeEnd's macro identification
    hoe::Log("  vtable ColosseumExtra +0x%llX, Colosseum +0x%llX",
             (unsigned long long)vtExtra, (unsigned long long)vtColos);

    // The 2D pass we borrow to get a render-phase draw. We want the SLOT's own
    // address so it can be rewritten, not the function it currently points at.
    if (uintptr_t vtDraw2D = FindVTable(off::TYPENAME_ACTION_DRAW_2D)) {
        r.Draw2DVtableSlot = vtDraw2D + (uintptr_t)off::SLOT_DRAW_2D_PASS * 8;
        Check("Draw2DPass", Slot(vtDraw2D, off::SLOT_DRAW_2D_PASS), off::EXPECT_DRAW_2D_PASS);
    } else {
        hoe::Log("  CActionDraw2D vtable not found - the draw hook will be skipped");
    }

    r.Step2CStub    = Slot(vtExtra, off::SLOT_STEP_2C_STUB);
    r.Step2DStub    = Slot(vtExtra, off::SLOT_STEP_2D_STUB);
    r.RefStep2C     = Slot(vtColos, off::SLOT_REF_STEP_2C);
    r.Step2DWorking = Slot(vtColos, off::SLOT_STEP_2D_WORKING);

    Check("Step2CStub",    r.Step2CStub,    off::EXPECT_STEP_2C_STUB);
    Check("Step2DStub",    r.Step2DStub,    off::EXPECT_STEP_2D_STUB);
    Check("RefStep2C",     r.RefStep2C,     off::EXPECT_REF_STEP_2C);
    Check("Step2DWorking", r.Step2DWorking, off::EXPECT_STEP_2D_WORKING);

    // The patch sites must still be `ret 0` stubs; if Sega ever fills them in,
    // this mod is obsolete and must not clobber real code.
    const unsigned char* img = Img();
    for (uintptr_t site : { r.Step2CStub, r.Step2DStub }) {
        if (!InImage(site) || img[site] != 0xC2 || img[site + 1] != 0x00 || img[site + 2] != 0x00) {
            hoe::Log("patch site +0x%llX is NOT a `ret 0` stub - refusing to patch",
                     (unsigned long long)site);
            return r;
        }
    }

    // --- anchor: cross-check the pattern agrees with the RTTI result ---
    uintptr_t anchor = FindPattern(off::ANCHOR_PATTERN);
    if (!anchor) { hoe::Log("anchor pattern not found"); return r; }
    if (anchor != r.RefStep2C) {
        hoe::Log("anchor +0x%llX disagrees with RTTI slot +0x%llX - refusing",
                 (unsigned long long)anchor, (unsigned long long)r.RefStep2C);
        return r;
    }

    // --- read the engine helpers and globals out of the anchor ---
    r.TransitionSetup = ReadCall(anchor + off::CALLAT_TRANSITION_SETUP);
    r.CommitFlags     = ReadCall(anchor + off::CALLAT_COMMIT_FLAGS);
    r.OpenScreen      = ReadCall(anchor + off::CALLAT_OPEN_SCREEN);
    r.SetNextStep     = ReadCall(anchor + off::CALLAT_SET_NEXT_STEP);
    r.PostOpenNotify  = ReadCall(anchor + off::CALLAT_POST_OPEN_NOTIFY);
    r.GFlagMgr = ReadRipRef(anchor + off::RIPAT_G_FLAG_MGR, off::RIPLEN_G_FLAG_MGR);
    r.GMainMgr = ReadRipRef(anchor + off::RIPAT_G_MAIN_MGR, off::RIPLEN_G_MAIN_MGR);
    r.KFloat   = ReadRipRef(anchor + off::RIPAT_K_FLOAT,    off::RIPLEN_K_FLOAT);

    Check("TransitionSetup", r.TransitionSetup, off::EXPECT_TRANSITION_SETUP);
    Check("CommitFlags",     r.CommitFlags,     off::EXPECT_COMMIT_FLAGS);
    Check("OpenScreen",      r.OpenScreen,      off::EXPECT_OPEN_SCREEN);
    Check("SetNextStep",     r.SetNextStep,     off::EXPECT_SET_NEXT_STEP);
    Check("PostOpenNotify",  r.PostOpenNotify,  off::EXPECT_POST_OPEN_NOTIFY);
    Check("GFlagMgr",        r.GFlagMgr,        off::EXPECT_G_FLAG_MGR);
    Check("GMainMgr",        r.GMainMgr,        off::EXPECT_G_MAIN_MGR);
    Check("KFloat",          r.KFloat,          off::EXPECT_K_FLOAT);

    if (!r.TransitionSetup || !r.CommitFlags || !r.OpenScreen ||
        !r.SetNextStep || !r.PostOpenNotify ||
        !r.GFlagMgr || !r.GMainMgr || !r.KFloat) {
        hoe::Log("one or more helpers failed to resolve");
        return r;
    }

    r.SetResultVariant = FindPattern(off::PATTERN_SET_RESULT_VARIANT);
    r.DrawElement = FindPattern(off::PATTERN_DRAW_ELEMENT);
    // Three pane-tree helpers. All are TAIL- or body-anchored because five
    // functions in this family share their prologue byte for byte; the backup
    // distances are declared alongside the patterns in offsets.json.
    if (uintptr_t tail = FindPattern(off::PATTERN_SET_PANE_HIDDEN)) {
        r.SetPaneHidden = tail - 0x7D;
        Check("SetPaneHidden", r.SetPaneHidden, off::EXPECT_SET_PANE_HIDDEN);
    }
    if (uintptr_t tail = FindPattern(off::PATTERN_PANE_POS_REG)) {
        r.PanePosReg = tail - 0x82;
        Check("PanePosReg", r.PanePosReg, off::EXPECT_PANE_POS_REG);
    }
    if (uintptr_t mid = FindPattern(off::PATTERN_PANE_EVAL)) {
        r.PaneEval = mid - 0x1C;
        Check("PaneEval", r.PaneEval, off::EXPECT_PANE_EVAL);
    }
    if (uintptr_t mid = FindPattern(off::PATTERN_TAGHELP_DRAW)) {
        r.TagHelpDraw = mid - 0x83;   // the anchor sits inside the function
        Check("TagHelpDraw", r.TagHelpDraw, off::EXPECT_TAGHELP_DRAW);
    }
    r.FontStyleOpen  = FindPattern(off::PATTERN_FONT_STYLE_OPEN);
    Check("FontStyleOpen",  r.FontStyleOpen,  off::EXPECT_FONT_STYLE_OPEN);
    // The recap's sounds. Non-fatal: the panel is complete without them, and a
    // missing resolve just means silence plus a log line, exactly as before.
    r.SndPlay        = FindPattern(off::PATTERN_SND_PLAY);
    Check("SndPlay",        r.SndPlay,        off::EXPECT_SND_PLAY);
    r.SndStop        = FindPattern(off::PATTERN_SND_STOP);
    Check("SndStop",        r.SndStop,        off::EXPECT_SND_STOP);
    r.TalkMatch      = FindPattern(off::PATTERN_TALK_MATCH);   // diagnostics only
    Check("TalkMatch",      r.TalkMatch,      off::EXPECT_TALK_MATCH);
    r.ColoScene      = FindPattern(off::PATTERN_COLO_SCENE);
    Check("ColoScene",      r.ColoScene,      off::EXPECT_COLO_SCENE);
    r.FontStyleClose = FindPattern(off::PATTERN_FONT_STYLE_CLOSE);
    Check("FontStyleClose", r.FontStyleClose, off::EXPECT_FONT_STYLE_CLOSE);
    r.FlagGet     = FindPattern(off::PATTERN_FLAG_GET);
    r.FlagSetRaw  = FindPattern(off::PATTERN_FLAG_SET_RAW);
    r.FlagSetEdge = FindPattern(off::PATTERN_FLAG_SET_EDGE);

    // The emitted-quad counter, read via the texture gate that guards the emit.
    if (uintptr_t gate = FindPattern(off::ANCHOR_TEX_GATE_PATTERN)) {
        r.QuadCounter = ReadRipRef(gate + off::RIPAT_G_QUAD_COUNTER,
                                   off::RIPLEN_G_QUAD_COUNTER);
        Check("QuadCounter", r.QuadCounter, off::EXPECT_G_QUAD_COUNTER);
    } else {
        hoe::Log("texture gate not found - emitted-quad counter unavailable");
    }
    Check("SetResultVariant", r.SetResultVariant, off::EXPECT_SET_RESULT_VARIANT);
    if (!r.SetResultVariant) {
        hoe::Log("result-variant setter not found - the screen would render nothing");
        return r;
    }

    // --- secondary anchors: the surviving DLC -> save-flag bridge ---
    // Bob's menu is a talk-script select whose rows are gated on save flags in
    // group 332. The PC build still ships the code that sets them from DLC
    // entitlements, but never grants entitlements 0x17..0x19, so four rows can
    // never appear. We reuse the game's own setter rather than poking bits.
    //
    // Deliberately non-fatal: the mode-completion fix is independent and
    // already works, so a miss here costs the unlock, not the whole mod.
    if (uintptr_t bridge = FindPattern(off::ANCHOR_DLC_FLAG_BRIDGE_PATTERN)) {
        r.SetFlagAlias = ReadCall(bridge + off::CALLAT_SET_FLAG_ALIAS);
        r.IsDlcOwned   = ReadCall(bridge + off::CALLAT_IS_DLC_OWNED);
        Check("SetFlagAlias", r.SetFlagAlias, off::EXPECT_SET_FLAG_ALIAS);
        Check("IsDlcOwned",   r.IsDlcOwned,   off::EXPECT_IS_DLC_OWNED);

        // The bitset address lives inside IsDlcOwned's own lea, not in the
        // pointer the bridge passes it (which the callee ignores outright).
        uintptr_t owned = FindPattern(off::ANCHOR_IS_DLC_OWNED_PATTERN);
        if (owned && owned == r.IsDlcOwned) {
            r.GDlcMask = ReadRipRef(owned + off::RIPAT_G_DLC_MASK, off::RIPLEN_G_DLC_MASK);
            Check("GDlcMask", r.GDlcMask, off::EXPECT_G_DLC_MASK);
        } else if (owned) {
            hoe::Log("  IsDlcOwned pattern +0x%llX disagrees with the bridge's call target +0x%llX",
                     (unsigned long long)owned, (unsigned long long)r.IsDlcOwned);
        }

        // We call SetFlagAlias blind, so verify it still is the thin
        // alias->(group,bit) decoder we reverse-engineered: read the manager's
        // definition table at +0x990, then tail-jump to the raw setter. A
        // reshaped function here would scribble on arbitrary save flags.
        bool shapeOk = false;
        if (InImage(r.SetFlagAlias)) {
            const unsigned char* f = img + r.SetFlagAlias;
            for (int i = 0; i < 0x50 && !shapeOk; ++i) {
                if (f[i] == 0x48 && f[i + 1] == 0x8B && f[i + 2] == 0x81) {
                    uint32_t disp;
                    memcpy(&disp, f + i + 3, 4);
                    shapeOk = (disp == (uint32_t)off::C_FLAGMGR_TABLE_FIELD);
                }
            }
        }
        if (!shapeOk) {
            hoe::Log("  flag setter does not read flagMgr+0x%llX - refusing to call it",
                     (unsigned long long)off::C_FLAGMGR_TABLE_FIELD);
            r.SetFlagAlias = 0;
        }

        r.unlockOk = InImage(r.SetFlagAlias) && InImage(r.GDlcMask);
    }
    if (!r.unlockOk) {
        hoe::Log("  mode unlock unavailable - only the rows the game already "
                 "shows will appear in Bob's menu");
    }

    // --- the network-ranking panel (optional, never fatal) ---
    // pjs_net_ranking is the layout the deleted src/ranking code drew. It still
    // ships, and so does the sibling widget for the minigame version of the same
    // panel, so we can drive it ourselves. NETRANK_BIND comes from its anchor
    // rather than a standalone pattern.
    r.HeapPush    = FindPattern(off::PATTERN_HEAP_PUSH);
    r.HeapAlloc   = FindPattern(off::PATTERN_HEAP_ALLOC);
    r.HeapPop     = FindPattern(off::PATTERN_HEAP_POP);
    r.LayoutLoad  = FindPattern(off::PATTERN_LAYOUT_LOAD);
    r.NetRankCtor = FindPattern(off::PATTERN_NETRANK_CTOR);
    r.NetRankShow = FindPattern(off::PATTERN_NETRANK_SHOW);
    r.NetRankDraw = FindPattern(off::PATTERN_NETRANK_DRAW);
    r.NetRankBind = FindPattern(off::ANCHOR_NETRANK_BIND_PATTERN);
    r.BuildPages    = FindPattern(off::PATTERN_BUILD_PAGES);
    r.GetPage       = FindPattern(off::PATTERN_GET_PAGE);
    r.SetSuppress   = FindPattern(off::PATTERN_SET_SUPPRESS);
    r.HeapFree      = FindPattern(off::PATTERN_HEAP_FREE);
    r.LayoutRelease = FindPattern(off::PATTERN_LAYOUT_RELEASE);
    if (uintptr_t sc = FindPattern(off::ANCHOR_LAYOUT_SLOT_COUNT_READ_PATTERN)) {
        r.GLayoutSlotCount = ReadRipRef(sc + off::RIPAT_G_LAYOUT_SLOT_COUNT,
                                        off::RIPLEN_G_LAYOUT_SLOT_COUNT);
        Check("GLayoutSlotCount", r.GLayoutSlotCount, off::EXPECT_G_LAYOUT_SLOT_COUNT);
    }
    Check("BuildPages",    r.BuildPages,    off::EXPECT_BUILD_PAGES);
    Check("GetPage",       r.GetPage,       off::EXPECT_GET_PAGE);
    Check("SetSuppress",   r.SetSuppress,   off::EXPECT_SET_SUPPRESS);
    Check("HeapFree",      r.HeapFree,      off::EXPECT_HEAP_FREE);
    Check("LayoutRelease", r.LayoutRelease, off::EXPECT_LAYOUT_RELEASE);
    Check("HeapPush",     r.HeapPush,     off::EXPECT_HEAP_PUSH);
    Check("HeapAlloc",    r.HeapAlloc,    off::EXPECT_HEAP_ALLOC);
    Check("HeapPop",      r.HeapPop,      off::EXPECT_HEAP_POP);
    Check("LayoutLoad",   r.LayoutLoad,   off::EXPECT_LAYOUT_LOAD);
    Check("NetRankCtor",  r.NetRankCtor,  off::EXPECT_NETRANK_CTOR);
    Check("NetRankBind",  r.NetRankBind,  off::EXPECT_NETRANK_BIND);
    Check("NetRankShow",  r.NetRankShow,  off::EXPECT_NETRANK_SHOW);
    Check("NetRankDraw",  r.NetRankDraw,  off::EXPECT_NETRANK_DRAW);
    // NetRankBind/Show/Draw are deliberately NOT required: reusing that widget on
    // this layout overflows its 0x28-byte panel (see netrank.cpp). What the panel
    // actually needs is the generic layout path.
    r.netRankOk = InImage(r.HeapPush) && InImage(r.HeapAlloc) && InImage(r.HeapPop) &&
                  InImage(r.LayoutLoad) && InImage(r.BuildPages) &&
                  InImage(r.GetPage) && InImage(r.SetSuppress) &&
                  InImage(r.HeapFree) && InImage(r.LayoutRelease) &&
                  InImage(r.GLayoutSlotCount);
    if (!r.netRankOk) hoe::Log("  net-ranking panel unavailable (non-fatal)");

    // --- the texture-residency gate (optional; see resolve.h) ---
    r.IsTexResident = FindPattern(off::PATTERN_IS_TEX_RESIDENT);
    Check("IsTexResident", r.IsTexResident, off::EXPECT_IS_TEX_RESIDENT);
    if (uintptr_t ctor = FindPattern(off::ANCHOR_PAGE_CTOR_PATTERN)) {
        Check("PageCtor", ctor, off::EXPECT_PAGE_CTOR);
        r.PageCtor     = ctor;
        r.SharedTexB   = ReadCall(ctor + off::CALLAT_SHARED_TEX_B);
        r.SetTexHandle = ReadCall(ctor + off::CALLAT_SET_TEX_HANDLE);
        Check("SharedTexB",   r.SharedTexB,   off::EXPECT_SHARED_TEX_B);
        Check("SetTexHandle", r.SetTexHandle, off::EXPECT_SET_TEX_HANDLE);
        r.SetAnimVariant = ReadCall(ctor + off::CALLAT_SET_ANIM_VARIANT);
        Check("SetAnimVariant", r.SetAnimVariant, off::EXPECT_SET_ANIM_VARIANT);

        // The two shared-id getters are adjacent 7-byte `mov eax,[rip+d]; ret`
        // stubs in the same aligned block, and neither has a prefix unique
        // enough to pattern-match on its own. Deriving the sibling by offset is
        // only safe if it still looks like that stub, so check the opcode
        // rather than trusting the layout.
        const uintptr_t sib = r.SharedTexB + 0x20;
        const unsigned char* p = (const unsigned char*)(hoe::Base() + sib);
        if (InImage(sib) && p[0] == 0x8B && p[1] == 0x05) {
            r.SharedTexA = sib;
        } else {
            hoe::Log("  SharedTexA: +0x20 from SharedTexB is not a mov/ret stub "
                     "(%02X %02X) - skipping that fallback", p[0], p[1]);
        }
    }
    r.texGateOk = InImage(r.IsTexResident) && InImage(r.SetTexHandle) &&
                  InImage(r.SharedTexB);
    if (!r.texGateOk) {
        hoe::Log("  texture gate helpers unavailable - the panel will build but "
                 "stay invisible (non-fatal)");
    }

    // --- the layout text API (optional; see resolve.h) ---
    if (uintptr_t td = FindPattern(off::ANCHOR_TEXT_API_PATTERN)) {
        Check("TextApiAnchor", td, off::EXPECT_TEXT_API);
        r.TextBind   = ReadCall(td + off::CALLAT_TEXT_BIND);
        r.TextPrintf = ReadCall(td + off::CALLAT_TEXT_PRINTF);
        r.GTextCtx   = ReadRipRef(td + off::RIPAT_G_TEXT_CTX, off::RIPLEN_G_TEXT_CTX);
        Check("TextBind",   r.TextBind,   off::EXPECT_TEXT_BIND);
        Check("TextPrintf", r.TextPrintf, off::EXPECT_TEXT_PRINTF);
        Check("GTextCtx",   r.GTextCtx,   off::EXPECT_G_TEXT_CTX);
    }
    r.textOk = InImage(r.TextBind) && InImage(r.TextPrintf) && InImage(r.GTextCtx);
    if (!r.textOk) {
        hoe::Log("  layout text API unavailable - the panel will draw its "
                 "artwork but no dynamic strings (non-fatal)");
    }

    // --- mission manager (diagnostics only, never fatal) ---
    if (uintptr_t tick = FindPattern(off::ANCHOR_MISSION_TICK_PATTERN)) {
        Check("MissionTick", tick, off::EXPECT_MISSION_TICK);
        r.GMissionMgr = ReadRipRef(tick + off::RIPAT_G_MISSION_MGR,
                                   off::RIPLEN_G_MISSION_MGR);
        Check("GMissionMgr", r.GMissionMgr, off::EXPECT_G_MISSION_MGR);
        r.missionDiagOk = InImage(r.GMissionMgr);
    }
    if (!r.missionDiagOk) hoe::Log("  mission watcher unavailable (non-fatal)");

    // --- layout introspection (diagnostics only, never fatal) ---
    // LoadLayout builds only "<name>.csb"; a layout's textures come from a
    // separate archive loaded by TEXPAR_LOAD into its own slot table. That
    // split is the leading explanation for a screen that opens, registers,
    // animates and holds while rendering nothing at all, so we report it.
    if (uintptr_t bind = FindPattern(off::ANCHOR_NETRANK_BIND_PATTERN)) {
        r.LayoutHasPages  = ReadCall(bind + off::CALLAT_LAYOUT_IS_LOADING);
        r.LayoutPageReady = ReadCall(bind + off::CALLAT_LAYOUT_PAGE_READY);
        r.LayoutGetPane   = ReadCall(bind + off::CALLAT_LAYOUT_GET_PANE);
        r.GLayoutRes      = ReadRipRef(bind + off::RIPAT_G_LAYOUT_RES,
                                       off::RIPLEN_G_LAYOUT_RES);
        Check("LayoutHasPages",  r.LayoutHasPages,  off::EXPECT_LAYOUT_IS_LOADING);
        Check("LayoutPageReady", r.LayoutPageReady, off::EXPECT_LAYOUT_PAGE_READY);
        Check("LayoutGetPane",   r.LayoutGetPane,   off::EXPECT_LAYOUT_GET_PANE);
        Check("GLayoutRes",      r.GLayoutRes,      off::EXPECT_G_LAYOUT_RES);

        if (uintptr_t tex = FindPattern(off::PATTERN_TEXPAR_LOAD)) {
            Check("TexParLoad", tex, off::EXPECT_TEXPAR_LOAD);
            r.GLayoutTexPar = ReadU32At(tex + off::C_TEXPAR_TABLE_DISP_AT + 4);
            Check("GLayoutTexPar", r.GLayoutTexPar, off::C_G_LAYOUT_TEXPAR_EXPECT);
        }

        if (uintptr_t nm = FindPattern(off::ANCHOR_LAYOUT_NAMES_READ_PATTERN)) {
            Check("LayoutNamesRead", nm, off::EXPECT_LAYOUT_NAMES_READ);
            r.GLayoutNames = ReadRipRef(nm + off::RIPAT_G_LAYOUT_NAMES,
                                        off::RIPLEN_G_LAYOUT_NAMES);
            Check("GLayoutNames", r.GLayoutNames, off::EXPECT_G_LAYOUT_NAMES);
        }

        r.layoutDiagOk = InImage(r.LayoutHasPages) && InImage(r.LayoutPageReady) &&
                         InImage(r.LayoutGetPane)  && InImage(r.GLayoutRes) &&
                         InImage(r.GLayoutTexPar);
    }
    if (!r.layoutDiagOk) hoe::Log("  layout diagnostics unavailable (non-fatal)");

    r.ok = true;
    hoe::Log("resolve OK");
    return r;
}

}  // namespace

bool WaitForText(int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 250) {
        if (FindPattern(off::ANCHOR_PATTERN) != 0) {
            hoe::Log(".text decrypted after ~%d ms", waited);
            return true;
        }
        Sleep(250);
    }
    return false;
}

const Resolved& Get() {
    static Resolved cached = Build();
    return cached;
}

}  // namespace resolve
