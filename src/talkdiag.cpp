// DiagTalkMatch: what does the engine's talk matcher actually see for Bob?
//
// Bob's stage pac (pac_STID_ST_URANAI.bin, object uid 00ee05b6) carries nine
// talk entries in file order: a PC-only dummy, "nothing to say", four
// protagonist greetings, and three post-mode lines gated on the extras flags
// (332:28 & 332:29 -> "What, done already?", 166:0 & 332:29 -> "enjoy",
// 332:29 -> "enjoy" variant). The matcher (+0xBA7AA0) stores the FIRST match
// per slot in entry order, and every Bob entry is slot 0 - so, read from the
// file, a greeting should always win... and so should the dummy, which has no
// condition at all, yet the game shows the greetings. The runtime view of the
// block therefore differs from the file (the PC loader byte-swaps and
// relocates it), and no amount of static reading settles it. This module logs
// the runtime view: entry order, each condition with its live flag value, and
// the slots the matcher returned. It is a detour - the prologue is 16
// relocatable bytes, copied into a trampoline that resumes at +0x10 - and it
// changes nothing: the original runs first, untouched, and we only read.
//
// Only blocks whose conditions mention the extras flags are logged, only when
// the outcome changes, and never more than kMaxLines lines per process.
#include "talkdiag.h"
#include "patch.h"
#include "resolve.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace talkdiag {
namespace {

using MatchFn = void (*)(void* ctx, uint16_t* out, void* block, void* p4);

MatchFn     s_orig      = nullptr;   // the trampoline
hoe::Patch  s_patch;
uintptr_t   s_rva       = 0;
int         s_lines     = 0;
uint32_t    s_calls     = 0;
constexpr int kMaxLines = 4000;

// The 16 bytes at +0xBA7AA0 that the trampoline relocates verbatim:
//   mov [rsp+0x10], rdx ; push r13 ; push r15 ; sub rsp, 0x98
// None of them is RIP-relative, so they run unchanged from anywhere.
constexpr unsigned char kPrologue[16] = {
    0x48, 0x89, 0x54, 0x24, 0x10, 0x41, 0x55, 0x41, 0x57,
    0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00 };

bool Sane(const void* p) {
    const uintptr_t v = (uintptr_t)p;
    return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
}

// A range check only. The first build asked the kernel (VirtualQuery) for every
// pointer on every call, and the matcher runs ~400 times a second across every
// NPC in town: the game dropped to 5 fps. The engine has just dereferenced these
// very pointers itself, so a cheap sanity bound is all this needs.
bool Readable(const void* p, size_t n) {
    return Sane(p) && Sane((const unsigned char*)p + n);
}

// The matcher's own walk, little-endian as the loader left it:
//   node    = (block+8) + i32[block+8]     count = u8[node+2]
//   entries = (node+4)  + i32[node+4]      entry = entries + 16*i
//   conds   = entry + i32[entry]           ncond = u8[entry+8]
//   cond row (12 B): op u32, group u16, bit u16, value u32
struct Cond { uint32_t op; uint16_t group, bit; uint32_t value; };

const unsigned char* Rel(const unsigned char* at) {
    const int32_t d = *(const int32_t*)at;
    return d ? at + d : nullptr;
}

// Exactly the gates on Bob's three post-mode entries: 332:28 (abort), 332:29
// (end) and 166:0. Nothing wider: the protagonist flags (1:31..34) are tested
// by every NPC in town, and 332:20..27 / 199:6..9 by eight other palace objects
// (the ranking boards) - the second build spent its whole line budget on those
// eight before the mode had even started.
bool IsExtrasFlag(uint16_t g, uint16_t b) {
    return (g == 332 && (b == 28 || b == 29)) || (g == 166 && b == 0);
}

// live value of a flag, or -1 if the bank is not up
int Live(uint16_t g, uint16_t b) {
    void* fm = game::pFlagMgr ? *game::pFlagMgr : nullptr;
    if (!fm || !game::FlagGet) return -1;
    return game::FlagGet(fm, g, b, 1) ? 1 : 0;
}

void Emit(const char* fmt, ...) {
    if (s_lines >= kMaxLines) return;
    char buf[640];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hoe::Log("%s", buf);
    if (++s_lines == kMaxLines) hoe::Log("talk-match: line budget spent - silent from here");
}

// Per-block memory so a block is logged only when its outcome changes.
struct Seen { const void* block; uint64_t sig; uint32_t calls; };
Seen s_seen[64] = {};

Seen* Slot(const void* block) {
    for (auto& s : s_seen) if (s.block == block) return &s;
    for (auto& s : s_seen) if (!s.block) { s.block = block; s.sig = ~0ull; s.calls = 0; return &s; }
    // more than 64 interesting blocks alive: recycle the first
    s_seen[0] = { block, ~0ull, 0 };
    return &s_seen[0];
}

}  // namespace

extern "C" __attribute__((ms_abi))
void HoE_TalkMatch(void* ctx, uint16_t* out, void* block, void* p4) {
    // The engine first, exactly as before; we only look afterwards.
    s_orig(ctx, out, block, p4);
    ++s_calls;
    if (s_lines >= kMaxLines) return;

    const auto* b = (const unsigned char*)block;
    if (!Readable(b, 12)) return;
    const unsigned char* node = Rel(b + 8);
    if (!Readable(node, 8)) return;
    const int count = node[2];
    if (count == 0) return;
    const unsigned char* entries = Rel(node + 4);
    if (!Readable(entries, (size_t)count * 16)) return;

    // Is this a block we care about, and what is its outcome right now?
    bool interesting = false;
    uint64_t sig = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) { sig ^= v; sig *= 0x100000001b3ull; };
    for (int i = 0; i < count; ++i) {
        const unsigned char* e = entries + 16 * i;
        const int ncond = e[8];
        const unsigned char* conds = Rel(e);
        if (ncond && !Readable(conds, (size_t)ncond * 12)) return;
        for (int j = 0; j < ncond; ++j) {
            const Cond* c = (const Cond*)(conds + 12 * j);
            if (c->op == 3 && IsExtrasFlag(c->group, c->bit)) {
                interesting = true;
                mix(((uint64_t)c->group << 32) | ((uint64_t)c->bit << 8) | (uint64_t)(Live(c->group, c->bit) + 1));
            }
        }
    }
    if (!interesting) return;
    for (int k = 0; k < 4; ++k) mix(out ? out[k] : 0xFFFF);

    Seen* s = Slot(block);
    ++s->calls;
    if (s->sig == sig) {
        // Unchanged outcome: a heartbeat every 600 calls (about 10 s if this
        // runs per frame) so the log still shows the block being re-evaluated.
        if (s->calls % 600 == 0)
            Emit("talk-match: block %p re-evaluated %u times, unchanged | slots %u %u %u %u | from +0x%llX",
                 block, s->calls, out ? out[0] : 0xFFFF, out ? out[1] : 0xFFFF, out ? out[2] : 0xFFFF, out ? out[3] : 0xFFFF,
                 (unsigned long long)((uintptr_t)__builtin_return_address(0) - hoe::Base()));
        return;
    }
    const uint32_t since = s->calls; s->calls = 0;
    s->sig = sig;

    const uintptr_t ret = (uintptr_t)__builtin_return_address(0) - hoe::Base();
    const int level = Readable((const unsigned char*)ctx + 0x288, 4) ? *(const int*)((const unsigned char*)ctx + 0x288) : -1;
    Emit("talk-match: block %p node %p entries %p count %d | ctx %p level %d p4 %p | from +0x%llX | %u calls since last line (%u total)",
         block, node, entries, count, ctx, level, p4, (unsigned long long)ret, since, s_calls);
    for (int i = 0; i < count; ++i) {
        const unsigned char* e = entries + 16 * i;
        const int ncond = e[8];
        const unsigned char* conds = Rel(e);
        char line[512]; int n = 0;
        n += snprintf(line + n, sizeof line - n, "  [%d] kind %02X%02X id %04X flags %02X %02X %02X %02X ncond %d:",
                      i, e[8], e[9], *(const uint16_t*)(e + 10), e[12], e[13], e[14], e[15], ncond);
        for (int j = 0; j < ncond && n < (int)sizeof line - 64; ++j) {
            const Cond* c = (const Cond*)(conds + 12 * j);
            if (c->op == 3) {
                const int live = Live(c->group, c->bit);
                const bool pass = live >= 0 && (live != 0) == (c->value != 0);
                n += snprintf(line + n, sizeof line - n, " %u:%u==%u(live %d %s)",
                              c->group, c->bit, c->value, live, live < 0 ? "?" : pass ? "ok" : "FAIL");
            } else {
                n += snprintf(line + n, sizeof line - n, " op%u(%04X,%04X,%08X)", c->op, c->group, c->bit, c->value);
            }
        }
        Emit("%s", line);
    }
    if (out) Emit("  -> slots %u %u %u %u", out[0], out[1], out[2], out[3]);
    else     Emit("  -> out is null");
}

bool Install() {
    if (!config::Get().DiagTalkMatch) return false;
    if (s_orig) return true;
    const auto& r = resolve::Get();
    if (!r.TalkMatch) { hoe::Log("talk-match: matcher unresolved - not installed"); return false; }
    s_rva = r.TalkMatch;
    const unsigned char* at = (const unsigned char*)(hoe::Base() + s_rva);
    if (memcmp(at, kPrologue, sizeof kPrologue) != 0) {
        hoe::Log("talk-match: prologue at +0x%llX is not the 16 bytes we relocate - not installed", (unsigned long long)s_rva);
        return false;
    }

    // Trampoline: the 16 original bytes, then `jmp [rip+0]` to +0x10 of the
    // matcher. Absolute jumps on both legs, so neither side needs to be near
    // the other.
    auto* tr = (unsigned char*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { hoe::Log("talk-match: no page for the trampoline - not installed"); return false; }
    memcpy(tr, kPrologue, sizeof kPrologue);
    const unsigned char jmpRip[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    memcpy(tr + 16, jmpRip, 6);
    const uintptr_t resume = hoe::Base() + s_rva + sizeof kPrologue;
    memcpy(tr + 22, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), tr, 32);

    // The detour itself: `jmp [rip+0]` + our address, padded with int3 to the
    // 16-byte prologue we replaced (nothing branches into those bytes).
    unsigned char p[16] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    const uintptr_t ours = (uintptr_t)&HoE_TalkMatch;
    memcpy(p + 6, &ours, 8);
    p[14] = 0xCC; p[15] = 0xCC;
    s_orig = (MatchFn)tr;
    if (!s_patch.Apply(s_rva, p, sizeof p, "talk matcher -> HoE_TalkMatch")) {
        s_orig = nullptr;
        VirtualFree(tr, 0, MEM_RELEASE);
        return false;
    }
    hoe::Log("talk-match: detour on +0x%llX, trampoline %p resumes at +0x%llX",
             (unsigned long long)s_rva, tr, (unsigned long long)(s_rva + sizeof kPrologue));
    return true;
}

}  // namespace talkdiag
