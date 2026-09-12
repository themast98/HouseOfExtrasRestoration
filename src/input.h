#pragma once

// Read-only pad state. No engine call is involved: the pad has already been
// sampled and updated earlier in the same frame by the time a mission step
// handler runs, so these values are final and immutable for the whole call.
//
// Validated live before this was written (probe D, 2026-08-30): a single
// confirm press produced exactly one TRG frame and exactly one OFF frame, and
// confirm/cancel were observed moving independently as bits 0 and 1.
namespace input {

struct Pad {
    unsigned on   = 0;   // held this frame
    unsigned trg  = 0;   // pressed this frame (rising edge)
    unsigned off  = 0;   // released this frame (falling edge)
    unsigned mask = 0;   // the confirm button's bit
    bool     ok   = false;
};

// Every dereference is guarded; ok is false if anything is missing.
Pad Read();

// True on either edge of the confirm button.
//
// Both edges are tested deliberately. The engine's own helper picks TRG or OFF
// depending on a device word whose writer was never found - that was the single
// unverified inference in this design. Requiring the button to be released
// before we arm (see recap.cpp) means the next edge can only be a fresh press,
// so OR-ing the two edges is exactly one event per press and the inference
// disappears.
bool ConfirmEdge(const Pad& p);

// True while the confirm button is held.
bool ConfirmHeld(const Pad& p);

}  // namespace input
