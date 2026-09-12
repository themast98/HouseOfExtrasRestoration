#include "input.h"
#include "offsets.h"
#include "patch.h"

namespace input {
namespace {

// A pointer we are about to chase has to look like one. A bad read here would
// fault inside a mission step handler, which is the worst place to find out.
bool Sane(const void* p) {
    const uintptr_t v = (uintptr_t)p;
    return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
}

}  // namespace

Pad Read() {
    Pad p;
    const uintptr_t base = hoe::Base();

    // g_padMgr -> slots[0] + 0x16D0 is the PadState block: three parallel
    // masks at +0x00 (ON), +0x04 (TRG) and +0x08 (OFF). The manager global is
    // read at sub_6EBF30+0x9D, immediately before the frame function.
    auto pm = *(unsigned char**)(base + off::C_G_PAD_MGR_RVA);
    if (!Sane(pm)) return p;
    if (*(const unsigned long long*)(pm + off::C_PAD_MGR_COUNT) == 0) return p;

    auto arr = *(unsigned char***)(pm + off::C_PAD_MGR_ARRAY);
    if (!Sane(arr)) return p;
    auto pad = arr[0];
    if (!Sane(pad)) return p;
    pad += off::C_PAD_STATE_OFFSET;

    // Which bit means "confirm" is a global, not a constant: it swaps with the
    // region/button-swap setting, so hard-coding cross or circle would be wrong
    // on some machines. Measured as 0 (confirm) and 1 (cancel) here.
    const unsigned bit = *(const unsigned*)(base + off::C_G_CONFIRM_BIT_RVA);
    if (bit > 31) return p;

    p.mask = 1u << bit;
    p.on   = *(const unsigned*)(pad + 0x00);
    p.trg  = *(const unsigned*)(pad + 0x04);
    p.off  = *(const unsigned*)(pad + 0x08);
    p.ok   = true;
    return p;
}

bool ConfirmEdge(const Pad& p) { return p.ok && ((p.trg | p.off) & p.mask) != 0; }
bool ConfirmHeld(const Pad& p) { return p.ok && (p.on & p.mask) != 0; }

}  // namespace input
