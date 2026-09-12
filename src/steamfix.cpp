#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "steamfix.h"
#include "config.h"
#include "log.h"
#include "offsets.h"
#include "patch.h"

namespace {

// Is the page holding p committed and readable? Used to guarantee the copy below
// never repeats the very fault it exists to prevent, in the pathological case
// where the source string genuinely has no terminator before the page ends.
bool PageUsable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    return true;
}

// A rel32 call reaches +/-2GB, and our DLL is nowhere near the image, so the
// redirect has to land on a stub allocated close to the game module.
// VirtualAlloc'd RW first, then flipped to RX - never both at once.
void* AllocNearExec(uintptr_t anchor, size_t n) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity
                                                      : 0x10000;
    for (uintptr_t delta = gran; delta < 0x40000000u; delta += gran) {
        for (int up = 0; up < 2; ++up) {
            const uintptr_t addr = up ? anchor + delta : anchor - delta;
            if (up == 0 && addr > anchor) continue;      // wrapped below zero
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((void*)addr, &mbi, sizeof mbi)) continue;
            if (mbi.State != MEM_FREE) continue;
            void* got = VirtualAlloc((void*)(addr & ~(uintptr_t)(gran - 1)), n,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (got) return got;
        }
    }
    return nullptr;
}

hoe::Patch g_nameCall;
hoe::Patch g_descCall;
unsigned char* g_stub = nullptr;

struct Site {
    uintptr_t   rva;        // the E8 rel32 we rewrite
    uint32_t    size;       // the `mov edx, imm32` 15 bytes earlier
    const char* what;
    hoe::Patch* patch;
};

}  // namespace

// The replacement for snprintf(dst, count, "%s", src).
//
// Byte-at-a-time on purpose. A one-byte read can never straddle a page, which is
// the entire bug being worked around; the length cap (127 for the name field,
// 255 for the description) means this touches at most as much as the engine's
// own buffer can hold. It reproduces the original's contract exactly: at most
// count-1 characters plus a NUL, returning the number of characters written.
extern "C" __attribute__((ms_abi))
int HoE_AchvCopy(char* dst, unsigned int count, const char* fmt, const char* src) {
    if (!dst || count == 0) return 0;

    // Only "%s" is ever passed here, but if the game is ever patched to pass
    // something else, refusing is far better than silently mis-formatting.
    if (!fmt || fmt[0] != '%' || fmt[1] != 's' || fmt[2] != '\0') {
        dst[0] = '\0';
        return 0;
    }
    if (!src) src = "(null)";

    const unsigned int cap = count - 1;
    unsigned int n = 0;

    // The source's own first page has to be readable before we touch it at all.
    if (!PageUsable((const void*)((uintptr_t)src & ~(uintptr_t)0xFFF))) {
        dst[0] = '\0';
        return 0;
    }
    while (n < cap) {
        const char* p = src + n;
        // Crossing into a new page: verify it before reading a single byte of it.
        if ((((uintptr_t)p) & 0xFFF) == 0 && !PageUsable(p)) break;
        if (*p == '\0') break;
        ++n;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return (int)n;
}

bool InstallSteamAchievementFix() {
    if (!config::Get().FixSteamAchievementCrash) return false;
    if (g_stub) return true;

    Site sites[2] = {
        { off::C_STEAM_ACHV_CALL_NAME, 0x80,  "name",        &g_nameCall },
        { off::C_STEAM_ACHV_CALL_DESC, 0x100, "description", &g_descCall },
    };

    // Refuse to touch anything unless BOTH sites still look exactly like the
    // code we decoded: a rel32 call to the shared snprintf thunk, preceded 15
    // bytes earlier by the `mov edx, <field size>` that sets the buffer cap.
    for (const Site& s : sites) {
        const unsigned char* p = (const unsigned char*)(hoe::Base() + s.rva);
        if (p[0] != 0xE8) {
            hoe::Log("steamfix: +0x%llX is 0x%02X, not a call - not patching",
                     (unsigned long long)s.rva, p[0]);
            return false;
        }
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        const uintptr_t target = s.rva + 5 + (intptr_t)rel;
        if (target != off::C_STEAM_SNPRINTF_THUNK) {
            hoe::Log("steamfix: +0x%llX calls +0x%llX, expected the snprintf thunk "
                     "+0x%llX - not patching", (unsigned long long)s.rva,
                     (unsigned long long)target,
                     (unsigned long long)off::C_STEAM_SNPRINTF_THUNK);
            return false;
        }
        uint32_t imm;
        memcpy(&imm, p - 14, 4);
        if (p[-15] != 0xBA || imm != s.size) {
            hoe::Log("steamfix: %s site is not preceded by `mov edx, 0x%X` "
                     "- not patching", s.what, s.size);
            return false;
        }
    }

    g_stub = (unsigned char*)AllocNearExec(hoe::Base(), 0x1000);
    if (!g_stub) {
        hoe::Log("steamfix: no free page within 2GB of the image - not patching");
        return false;
    }
    void* fn = (void*)&HoE_AchvCopy;
    g_stub[0] = 0x48; g_stub[1] = 0xB8;            // mov rax, imm64
    memcpy(g_stub + 2, &fn, 8);
    g_stub[10] = 0xFF; g_stub[11] = 0xE0;          // jmp rax
    DWORD old = 0;
    if (!VirtualProtect(g_stub, 0x1000, PAGE_EXECUTE_READ, &old)) {
        hoe::Log("steamfix: could not make the stub executable - not patching");
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = nullptr;
        return false;
    }

    for (const Site& s : sites) {
        const int64_t rel = (int64_t)(uintptr_t)g_stub
                          - (int64_t)(hoe::Base() + s.rva + 5);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            hoe::Log("steamfix: stub is out of rel32 range - not patching");
            g_nameCall.Revert();
            return false;
        }
        unsigned char bytes[5] = { 0xE8 };
        const int32_t rel32 = (int32_t)rel;
        memcpy(bytes + 1, &rel32, 4);
        char label[64];
        snprintf(label, sizeof label, "steamfix: achievement %s copy", s.what);
        if (!s.patch->Apply(s.rva, bytes, sizeof bytes, label)) {
            g_nameCall.Revert();
            g_descCall.Revert();
            return false;
        }
    }

    hoe::Log("steamfix: achievement name/desc formatting routed through a "
             "page-safe copy (stub at %p) - works around a STOCK-GAME crash "
             "at +0x%llX, reproduced with every mod removed",
             (void*)g_stub, (unsigned long long)off::C_STEAM_STRLEN_FAULT);
    return true;
}
