#include "console.h"
#include "paths.h"
#include "log.h"
#include "game.h"
#include "offsets.h"
#include "patch.h"   // hoe::Base / hoe::ImageSize

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>   // Out() is variadic

namespace console {
namespace {

bool s_resume = false;
int  s_tick   = 0;

// ~4 polls a second at 60fps. Interactive enough, and keeps the per-frame cost
// of the frozen recap at zero for 14 frames out of 15.
constexpr int kPollEveryFrames = 15;

char s_out[16384];
size_t s_len = 0;

void Out(const char* fmt, ...) {
    if (s_len + 512 > sizeof(s_out)) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(s_out + s_len, sizeof(s_out) - s_len - 2, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += (size_t)n;
    if (s_len < sizeof(s_out) - 1) s_out[s_len++] = '\n';
    s_out[s_len] = 0;
}

// A committed page with the access we need. This is what keeps a mistyped
// address from taking the game down.
bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    auto a = (const unsigned char*)p;
    while (n) {
        if (!VirtualQuery(a, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
        if (mbi.Protect & bad) return false;
        auto end = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        const size_t got = (size_t)(end - a);
        if (got >= n) return true;
        n -= got;
        a = end;
    }
    return true;
}

bool Writable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & ok) != 0 && Readable(p, n);
}

uint64_t Hex(const char* s) { return (uint64_t)_strtoui64(s, nullptr, 16); }

void DoRead(uint64_t addr, uint64_t len) {
    if (len == 0 || len > 512) { Out("ERR len must be 1..512"); return; }
    if (!Readable((void*)addr, (size_t)len)) { Out("ERR %llX not readable", addr); return; }
    auto p = (const unsigned char*)addr;
    char line[3 * 16 + 1];
    for (uint64_t i = 0; i < len; i += 16) {
        const uint64_t n = (len - i < 16) ? len - i : 16;
        for (uint64_t k = 0; k < n; ++k) snprintf(line + k * 3, 4, "%02X ", p[i + k]);
        line[n * 3] = 0;
        Out("  %016llX  %s", addr + i, line);
    }
}

void DoCall(uint64_t rva, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    const uintptr_t base = hoe::Base();
    if (rva == 0 || rva >= hoe::ImageSize()) { Out("ERR rva 0x%llX outside the image", rva); return; }
    auto fn = (uint64_t(*)(uint64_t, uint64_t, uint64_t, uint64_t))(base + rva);
    // Logged BEFORE the call so a crash still leaves a record of what did it.
    hoe::Log("console: CALL +0x%llX(%llX, %llX, %llX, %llX)", rva, a1, a2, a3, a4);
    const uint64_t r = fn(a1, a2, a3, a4);
    Out("  call +0x%llX -> 0x%llX", rva, r);
    hoe::Log("console: CALL +0x%llX returned 0x%llX", rva, r);
}

// Only one thread may own the reply buffer at a time.
volatile LONG s_busy = 0;
bool s_allowMutation = false;

bool NeedGameThread(const char* what) {
    if (s_allowMutation) return false;
    Out("ERR '%s' needs the game thread - set RecapFreeze=1 and get to the recap "
        "first. Reads work from anywhere.", what);
    return true;
}

void Execute(char* line) {
    while (*line == ' ') ++line;
    if (!*line || *line == '#') return;

    char* tok[8] = {};
    int n = 0;
    for (char* p = strtok(line, " \t\r\n"); p && n < 8; p = strtok(nullptr, " \t\r\n")) tok[n++] = p;
    if (!n) return;

    if (!strcmp(tok[0], "read") && n >= 3) {
        DoRead(Hex(tok[1]), Hex(tok[2]));
    } else if (!strcmp(tok[0], "readrva") && n >= 3) {
        DoRead(hoe::Base() + Hex(tok[1]), Hex(tok[2]));
    } else if (!strcmp(tok[0], "poke32") && n >= 3) {
        if (NeedGameThread("poke32")) return;
        const uint64_t a = Hex(tok[1]);
        if (!Writable((void*)a, 4)) { Out("ERR %llX not writable", a); return; }
        const uint32_t old = *(uint32_t*)a;
        *(uint32_t*)a = (uint32_t)Hex(tok[2]);
        Out("  poke32 %llX: %08X -> %08X", a, old, *(uint32_t*)a);
        hoe::Log("console: poke32 %llX %08X -> %08X", a, old, *(uint32_t*)a);
    } else if (!strcmp(tok[0], "poke64") && n >= 3) {
        if (NeedGameThread("poke64")) return;
        const uint64_t a = Hex(tok[1]);
        if (!Writable((void*)a, 8)) { Out("ERR %llX not writable", a); return; }
        const uint64_t old = *(uint64_t*)a;
        *(uint64_t*)a = Hex(tok[2]);
        Out("  poke64 %llX: %016llX -> %016llX", a, old, *(uint64_t*)a);
        hoe::Log("console: poke64 %llX %016llX -> %016llX", a, old, *(uint64_t*)a);
    } else if (!strcmp(tok[0], "call") && n >= 2) {
        if (NeedGameThread("call")) return;
        DoCall(Hex(tok[1]),
               n > 2 ? Hex(tok[2]) : 0, n > 3 ? Hex(tok[3]) : 0,
               n > 4 ? Hex(tok[4]) : 0, n > 5 ? Hex(tok[5]) : 0);
    } else if (!strcmp(tok[0], "slots")) {
        game::LogAllLayoutSlots("console");
        Out("  slot dump written to the log");
    } else if (!strcmp(tok[0], "base")) {
        Out("  base %016llX  size 0x%llX", (unsigned long long)hoe::Base(),
            (unsigned long long)hoe::ImageSize());
    } else if (!strcmp(tok[0], "resume")) {
        s_resume = true;
        Out("  resuming - the recap will finish and return to the palace");
    } else {
        Out("ERR unknown command '%s'", tok[0]);
    }
}

}  // namespace

bool ResumeRequested() { return s_resume; }

void Pump(bool allowMutation) {
    // The game thread polls every N frames; the background thread calls in on
    // its own 2s heartbeat and must not rate-limit itself against that counter.
    if (allowMutation && ++s_tick < kPollEveryFrames) return;
    if (allowMutation) s_tick = 0;

    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return;
    struct Unlock { ~Unlock() { InterlockedExchange(&s_busy, 0); } } unlock;
    s_allowMutation = allowMutation;

    const char* cmdPath = hoe::ExeRelative("HouseOfExtras.cmd");
    const char* repPath = hoe::ExeRelative("HouseOfExtras.reply");
    if (!cmdPath || !repPath) return;

    FILE* f = fopen(cmdPath, "r");
    if (!f) return;
    char buf[4096];
    const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = 0;
    if (!got) return;

    // Consume it first: if a command crashes, the same command must not run
    // again on the next launch and crash us a second time.
    FILE* clear = fopen(cmdPath, "w");
    if (clear) fclose(clear);

    s_len = 0;
    s_out[0] = 0;
    hoe::Log("console: executing %d bytes of commands", (int)got);

    // Split the lines up front rather than iterating with strtok: Execute()
    // uses strtok itself, and strtok has one global cursor, so an outer
    // strtok loop would be hijacked by the first command it parsed.
    char* lines[64];
    int count = 0;
    for (char* p = buf; *p && count < 64;) {
        lines[count++] = p;
        char* nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }

    for (int i = 0; i < count; ++i) {
        char copy[512];
        snprintf(copy, sizeof(copy), "%s", lines[i]);
        Out("> %s", copy);
        Execute(copy);
    }

    FILE* rf = fopen(repPath, "w");
    if (rf) { fwrite(s_out, 1, s_len, rf); fclose(rf); }
}

}  // namespace console
