#include "console.h"
#include "paths.h"
#include "log.h"
#include "game.h"
#include "netrank.h"
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
    } else if (!strcmp(tok[0], "nr") && n >= 2) {
        // Rebuilding the panel in place. Without this, every experiment costs a
        // full Battle King run, because the panel is only ever constructed on
        // the way through step 0x2C. The frozen recap already calls
        // netrank::Ready() and Show() once per frame, so tearing the panel down
        // and re-opening it here is enough for the existing loop to rebuild it
        // from scratch - one run then yields unlimited iterations.
        if (NeedGameThread("nr")) return;
        if (!strcmp(tok[1], "close")) {
            netrank::Close();
            Out("  netrank: closed (open=%d)", (int)netrank::IsOpen());
        } else if (!strcmp(tok[1], "reopen")) {
            netrank::Close();
            const bool ok = netrank::Open();
            Out("  netrank: reopen -> %s; the recap loop will build and show it "
                "over the next frames", ok ? "open" : "FAILED");
        } else if (!strcmp(tok[1], "state")) {
            Out("  netrank: open=%d failed=%d", (int)netrank::IsOpen(),
                (int)netrank::Failed());
        } else if (!strcmp(tok[1], "keep") || !strcmp(tok[1], "nokeep")) {
            const bool on = !strcmp(tok[1], "keep");
            netrank::SetKeepOpen(on);
            Out("  netrank: keep-open %s%s", on ? "ON" : "off",
                on ? " - send `resume` and the panel should follow you out of "
                     "the recap into the palace, where 2D definitely composites"
                   : "");
        } else if (!strcmp(tok[1], "text") && n >= 6) {
            // nr text <slot> <page> <modeHex> <0|1>
            const int slot = (int)Hex(tok[2]);
            const int page = (int)Hex(tok[3]);
            const int mode = (int)Hex(tok[4]);
            const bool on  = Hex(tok[5]) != 0;
            if (!netrank::SetTextSlot(slot, page, mode, on)) {
                Out("ERR nr text: slot must be 0 (board) 1 (latest) 2 (best)");
            } else {
                Out("  slot %d -> page %d mode %#x %s", slot, page, mode, on ? "ON" : "off");
            }
        } else if (!strcmp(tok[1], "prio") && n >= 3) {
            netrank::SetDrawPriority((int)(long long)Hex(tok[2]));
            Out("  draw priority stamped - see the log");
        } else if (!strcmp(tok[1], "elems") && n >= 4) {
            netrank::LogElementList(Hex(tok[2]), Hex(tok[3]));
            Out("  element list written to the log");
        } else if (!strcmp(tok[1], "open")) {
            netrank::RequestOpen();
            Out("  queued: open the panel here (serviced on the game thread)");
        } else if (!strcmp(tok[1], "shut")) {
            netrank::RequestClose();
            Out("  queued: close the panel");
        } else if (!strcmp(tok[1], "publish")) {
            netrank::RequestPublish();
            Out("  queued: republish resolved texture ids to pane+0x4C");
        } else if (!strcmp(tok[1], "hide") && n >= 5) {
            netrank::HidePane((int)Hex(tok[2]), (unsigned short)Hex(tok[3]),
                              Hex(tok[4]) != 0);
            Out("  pane visibility queued - see the log");
        } else if (!strcmp(tok[1], "close") && n >= 3) {
            netrank::ShowClosePrompt(Hex(tok[2]) != 0);
            Out("  close prompt toggled - see the log");
        } else if (!strcmp(tok[1], "page") && n >= 4) {
            netrank::ShowPage((int)Hex(tok[2]), Hex(tok[3]) != 0);
            Out("  page visibility changed - see the log");
        } else if (!strcmp(tok[1], "font") && n >= 6) {
            // DECIMAL here, unlike the other verbs - -1 is a meaningful value
            // and these are all small numbers where hex would only mislead.
            netrank::SetNumberFont(atoi(tok[2]), atoi(tok[3]), atoi(tok[4]),
                                   atoi(tok[5]), n >= 7 ? atoi(tok[6]) : -1);
            Out("  glyph fields overridden - watch the panel and the log");
        } else if (!strcmp(tok[1], "fonttab")) {
            netrank::LogFontTables();
            Out("  per-bank glyph tables written to the log");
        } else if (!strcmp(tok[1], "panes") && n >= 3) {
            netrank::LogPanes((int)Hex(tok[2]));
            Out("  pane census written to the log");
        } else if (!strcmp(tok[1], "texgate")) {
            netrank::LogTextureGate();
            Out("  texture-residency numbers written to the log");
        } else if (!strcmp(tok[1], "slots")) {
            netrank::LogTextSlots();
            Out("  text slot table written to the log");
        } else if (!strcmp(tok[1], "name")) {
            // The board name has spaces in it ("Battle King Ranking"), and
            // strtok has already cut the line into words, so put it back
            // together from token 2 onwards rather than taking tok[2] alone.
            char joined[64] = {};
            size_t len = 0;
            for (int i = 2; i < n && len < sizeof(joined) - 1; ++i) {
                if (len) joined[len++] = ' ';
                for (const char* q = tok[i]; *q && len < sizeof(joined) - 1; ++q) {
                    joined[len++] = *q;
                }
            }
            joined[len] = 0;
            netrank::SetBoardName(joined);
            Out("  board name -> '%s'", joined);
        } else {
            Out("ERR nr: expected close | reopen | state | keep | nokeep | "
                "text <slot> <page> <modeHex> <0|1> | slots | name <text> | "
                "prio <hex> | elems <headRva> <countRva>");
        }
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

    const char* cmdPath = hoe::ModRelative("HouseOfExtras.cmd");
    const char* repPath = hoe::ModRelative("HouseOfExtras.reply");
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
