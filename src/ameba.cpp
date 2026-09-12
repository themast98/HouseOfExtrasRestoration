// AmebaFloor: the Ameba collaboration decal on the coliseum floor.
//
// On PS3, Battle King and Fastest Killer run on a coliseum whose floor carries
// the green "Ameba" logo (texture s_sud_tou_df000.dds, used only by scene
// object SO033 of st_tougi_scn033.gmd). The asset ships on PC too, byte for
// byte, but the object never appears. Stage scene objects are shown iff their
// NAMED flag is set - the sub-loader resolves "%s_SCN%03d" against the flag
// manager and gates on FlagGet - and SO033 hangs off STID_ST_TOUGI_SCN033
// (group 521 bit 12, alias 1242). Both platforms have a coliseum scene-flag
// function that picks which SCNxxx flags to raise from the current mode
// (PC +0x3927E0, PS3 0x6CAABC). Their branch tables agree, except for the tail:
//
//   PS3: SCN033 stays as the branch chose it (set in every branch but one) and
//        is cleared only when the P2C3 story flags 9:50 && 9:58 are both set.
//   PC:  every path falls into a tail that writes SCN003 = !166:40 and then
//        SCN033 = 0 unconditionally (+0x393709..+0x393723). Removed on purpose.
//
// So this module is a post-detour: the original runs untouched, then if the
// stage is ST_TOUGI (0x66) and a ranking coliseum mode is armed (any of
// NETWORK_RANKING 199:2..9 - the exact bits Bob's row sets to start Battle
// King / Fastest Killer) it raises SCN033 and lowers SCN003, through the same
// alias table and raw setter the game's own tail uses. Outside those modes the
// PC result stands, which matches the observable PS3 behaviour: the decal is
// "not present in the standard coliseum".
//
// The prologue is 16 relocatable bytes, copied into a trampoline that resumes
// at +0x10. Logging is change-only and bounded.
#include "ameba.h"
#include "patch.h"
#include "resolve.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace ameba {
namespace {

using SceneFn = void (*)(void* stageMgr);

SceneFn    s_orig  = nullptr;   // the trampoline
hoe::Patch s_patch;
uintptr_t  s_rva   = 0;
int        s_lines = 0;
int        s_last  = -1;        // last decision logged: -1 none, 0 left alone, 1 forced
constexpr int kMaxLines = 64;

constexpr int      kStageTougi  = 0x66;  // ST_TOUGI in the stage-name table (+0x16E5BD0)
constexpr int      kAliasScn033 = 1242;  // STID_ST_TOUGI_SCN033 (521:12): the Ameba floor
constexpr int      kAliasScn003 = 816;   // STID_ST_TOUGI_SCN003 (521:3): the regular emblem floor
constexpr uint16_t kNetRank     = 199;   // NETWORK_RANKING: 2..5 Battle King x4, 6..9 Fastest Killer x4

// The 16 bytes at +0x3927E0 that the trampoline relocates verbatim:
//   push rbx ; sub rsp, 0x20 ; movsxd rax, [rcx+0x1b0] ; mov rbx, rcx
// None of them is RIP-relative, and +0x10 is an instruction boundary
// (cmp eax, 0x61), so they run unchanged from anywhere.
constexpr unsigned char kPrologue[16] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0x81, 0xB0, 0x01, 0x00, 0x00,
    0x48, 0x8B, 0xD9 };

bool Sane(const void* p) {
    const uintptr_t v = (uintptr_t)p;
    return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
}

int Live(void* fm, int g, int b) { return game::FlagGet(fm, g, b, 1) ? 1 : 0; }

}  // namespace

extern "C" __attribute__((ms_abi))
void HoE_ColoScene(void* stageMgr) {
    // The engine first, exactly as before; we only add to its result.
    s_orig(stageMgr);
    if (!Sane(stageMgr)) return;
    const int stage = *(const int*)((const unsigned char*)stageMgr + 0x1b0);
    if (stage != kStageTougi) return;

    void* fm = game::pFlagMgr ? *game::pFlagMgr : nullptr;
    if (!fm || !game::FlagGet || !game::SetFlagAlias) return;

    unsigned armed = 0;
    for (int b = 2; b <= 9; ++b)
        if (Live(fm, kNetRank, b)) armed |= 1u << b;

    if (armed) {
        game::SetFlagAlias(fm, kAliasScn033, 1);   // show the Ameba floor
        game::SetFlagAlias(fm, kAliasScn003, 0);   // and hide the emblem floor under it
    }

    const int decision = armed ? 1 : 0;
    if (decision == s_last || s_lines >= kMaxLines) return;
    s_last = decision;
    ++s_lines;
    // The inputs PC's tail and PS3's tail disagree on, so a wrong floor can be
    // explained from the log alone: 166:40 (tournament I entry) picks branch
    // A on both; 9:50 && 9:58 is the PS3-only story override; bank says which
    // flag bank FlagGet is reading.
    const int bank = *(const int*)((const unsigned char*)fm + 0x9b0);
    hoe::Log("ameba: coliseum scene refresh, ranking bits 199:2..9 = 0x%03X -> %s | 166:40=%d 9:50=%d 9:58=%d bank=%d | now SCN033(521:12)=%d SCN003(521:3)=%d",
             armed, armed ? "Ameba floor ON" : "PC result kept",
             Live(fm, 166, 40), Live(fm, 9, 50), Live(fm, 9, 58), bank,
             Live(fm, 521, 12), Live(fm, 521, 3));
    if (s_lines == kMaxLines) hoe::Log("ameba: line budget spent - silent from here");
}

bool Install() {
    if (!config::Get().AmebaFloor) return false;
    if (s_orig) return true;
    const auto& r = resolve::Get();
    if (!r.ColoScene) { hoe::Log("ameba: coliseum scene function unresolved - not installed"); return false; }
    s_rva = r.ColoScene;
    const unsigned char* at = (const unsigned char*)(hoe::Base() + s_rva);
    if (memcmp(at, kPrologue, sizeof kPrologue) != 0) {
        hoe::Log("ameba: prologue at +0x%llX is not the 16 bytes we relocate - not installed", (unsigned long long)s_rva);
        return false;
    }

    // Trampoline: the 16 original bytes, then `jmp [rip+0]` to +0x10 of the
    // function. Absolute jumps on both legs, so neither side needs to be near
    // the other.
    auto* tr = (unsigned char*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { hoe::Log("ameba: no page for the trampoline - not installed"); return false; }
    memcpy(tr, kPrologue, sizeof kPrologue);
    const unsigned char jmpRip[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    memcpy(tr + 16, jmpRip, 6);
    const uintptr_t resume = hoe::Base() + s_rva + sizeof kPrologue;
    memcpy(tr + 22, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), tr, 32);

    // The detour itself: `jmp [rip+0]` + our address, padded with int3 to the
    // 16-byte prologue we replaced (nothing branches into those bytes).
    unsigned char p[16] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    const uintptr_t ours = (uintptr_t)&HoE_ColoScene;
    memcpy(p + 6, &ours, 8);
    p[14] = 0xCC; p[15] = 0xCC;
    s_orig = (SceneFn)tr;
    if (!s_patch.Apply(s_rva, p, sizeof p, "coliseum scene flags -> HoE_ColoScene")) {
        s_orig = nullptr;
        VirtualFree(tr, 0, MEM_RELEASE);
        return false;
    }
    hoe::Log("ameba: detour on +0x%llX, trampoline %p resumes at +0x%llX",
             (unsigned long long)s_rva, tr, (unsigned long long)(s_rva + sizeof kPrologue));
    return true;
}

}  // namespace ameba
