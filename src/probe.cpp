#include "probe.h"
#include "resolve.h"
#include "offsets.h"
#include "patch.h"
#include "log.h"

// ---------------------------------------------------------------------------
// A LOGGING-ONLY validation pass for the planned step-0x2C/0x2D restructure.
//
// Nothing here writes anything or calls into the engine. It exists because two
// separate conclusions in this project were drawn from comparisons whose two
// halves were gathered differently - a recap sample taken on the game thread at
// a fixed point in the frame against a palace sample taken from the background
// thread at an arbitrary one, and a "baseline" that turned out to be a boot
// loading screen. Every read below therefore happens at ONE place: inside the
// frozen step-0x2C handler, on the game thread, once per frame.
//
// What each block decides:
//   A  the step machinery - is a plain store to macro+0x12C really the whole
//      advance? (SetNextStep is literally `mov [rcx+0x12c], edx ; ret`.)
//   B  presentation - is the render-list root the NORMAL one while our panel is
//      up? mgr+0xBB8 == mgr+0xC78 means normal; +0xF58/+0x1010 mean suspended.
//   C  slot hygiene - screen 225's slot must stay NULL; we are not using it.
//   D  the pad path - the go/no-go for dismissing on a button with no engine
//      call at all.
// ---------------------------------------------------------------------------

namespace probe {
namespace {

int      s_frames   = 0;
unsigned s_lastOn   = 0xFFFFFFFFu;
unsigned s_lastTrg  = 0xFFFFFFFFu;
unsigned s_lastOff  = 0xFFFFFFFFu;
bool     s_headerDone = false;

// Anything we chase through must land inside the module or a plausible heap
// pointer; a bad read here would crash the very session we are measuring.
bool Sane(const void* p) {
    const uintptr_t v = (uintptr_t)p;
    return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
}

}  // namespace

void StepAndPresent(void* macro) {
    const auto& r = resolve::Get();
    const uintptr_t base = hoe::Base();
    if (!r.GMainMgr) return;

    // Throttled here rather than at the call site so the frame counter still
    // advances every frame: A/B/C are steady-state facts, not events.
    const bool speak = (s_frames % 120) == 0;
    ++s_frames;
    if (!s_headerDone) {
        s_headerDone = true;
        hoe::Log("probe: validating the step-0x2C/0x2D restructure (reads only)");
    }
    if (!speak) return;

    // --- A. step machinery ---------------------------------------------------
    if (Sane(macro)) {
        const int cur  = *(const int*)((const unsigned char*)macro + off::C_MACRO_STEP_CUR);
        const int pend = *(const int*)((const unsigned char*)macro + off::C_MACRO_STEP_PENDING);
        // Expect cur == 0x2C and pend == -1: the driver latches +0x12C into
        // +0x128 and rewrites +0x12C to -1 on a later pass, so a pending value
        // of -1 means the slot is free for us to write the advance into.
        hoe::Log("probe A: step cur=%#x pending=%d %s", cur, pend,
                 (cur == 0x2C && pend == -1) ? "(as expected)"
                                             : "<< UNEXPECTED - the advance plan is unsafe");
    }

    // --- B. presentation -----------------------------------------------------
    auto mgr = *(unsigned char**)(base + r.GMainMgr);
    if (Sane(mgr)) {
        const unsigned flags = *(const unsigned*)(mgr + off::C_MGR_SUSPEND_FLAGS);
        const unsigned pass  = *(const unsigned*)(mgr + off::C_MGR_SUSPEND_PASS);
        const void*    rl    = *(void* const*)   (mgr + off::C_MGR_RENDER_LIST);
        const void*    norm  = mgr + off::C_MGR_RENDER_LIST_NORMAL;
        hoe::Log("probe B: suspend=%#x pass=%u renderList=%p %s",
                 flags, pass, rl,
                 rl == norm ? "== mgr+0xC78 (NORMAL list)"
                            : "<< NOT the normal list - suspended or unknown");

        // --- C. slot hygiene -------------------------------------------------
        const void* slot225 =
            *(void* const*)(mgr + off::C_MGR_SCREEN_SLOT_BASE + 225 * 8);
        hoe::Log("probe C: screen225 slot=%p %s", slot225,
                 slot225 ? "<< NOT NULL - something opened it" : "(NULL, as intended)");
    }
}

void PadEdges() {
    const uintptr_t base = hoe::Base();

    // padMgr -> slots[0] + 0x16D0 gives three parallel masks: ON, TRG (pressed
    // this frame) and OFF (released this frame). Read-only; the engine has
    // already sampled and updated the pad earlier in this same frame, so the
    // values are final for the whole call.
    auto pm = *(unsigned char**)(base + off::C_G_PAD_MGR_RVA);
    if (!Sane(pm)) return;
    const unsigned long long count = *(const unsigned long long*)(pm + off::C_PAD_MGR_COUNT);
    if (!count) return;
    auto arr = *(unsigned char***)(pm + off::C_PAD_MGR_ARRAY);
    if (!Sane(arr)) return;
    auto pad = arr[0];
    if (!Sane(pad)) return;
    pad += off::C_PAD_STATE_OFFSET;

    const unsigned bit = *(const unsigned*)(base + off::C_G_CONFIRM_BIT_RVA);
    if (bit > 31) return;
    const unsigned mask = 1u << bit;

    const unsigned on  = *(const unsigned*)(pad + 0x00);
    const unsigned trg = *(const unsigned*)(pad + 0x04);
    const unsigned off_= *(const unsigned*)(pad + 0x08);

    // Only log transitions. A press produces exactly one TRG frame and one OFF
    // frame, and per-frame logging would bury them.
    if (on == s_lastOn && trg == s_lastTrg && off_ == s_lastOff) return;
    s_lastOn = on; s_lastTrg = trg; s_lastOff = off_;

    const unsigned cancel = *(const unsigned*)(base + off::C_G_CANCEL_BIT_RVA);
    hoe::Log("probe D: ON=%08X TRG=%08X OFF=%08X  confirmBit=%u(mask %08X) cancelBit=%u"
             "%s%s%s", on, trg, off_, bit, mask, cancel,
             (on  & mask) ? "  [HELD]"     : "",
             (trg & mask) ? "  [PRESSED]"  : "",
             (off_& mask) ? "  [RELEASED]" : "");
}

void Reset() {
    s_frames = 0;
    s_lastOn = s_lastTrg = s_lastOff = 0xFFFFFFFFu;
    s_headerDone = false;
}

int Frames() { return s_frames; }

}  // namespace probe
