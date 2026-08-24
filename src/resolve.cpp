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

    uintptr_t vtExtra = FindVTable(off::TYPENAME_COLOSSEUM_EXTRA);
    uintptr_t vtColos = FindVTable(off::TYPENAME_COLOSSEUM);
    if (!vtExtra || !vtColos) { hoe::Log("RTTI walk failed"); return r; }
    hoe::Log("  vtable ColosseumExtra +0x%llX, Colosseum +0x%llX",
             (unsigned long long)vtExtra, (unsigned long long)vtColos);

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
    Check("SetResultVariant", r.SetResultVariant, off::EXPECT_SET_RESULT_VARIANT);
    if (!r.SetResultVariant) {
        hoe::Log("result-variant setter not found - the screen would render nothing");
        return r;
    }

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
