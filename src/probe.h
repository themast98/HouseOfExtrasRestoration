#pragma once

// Logging-only validation of the planned step-0x2C/0x2D restructure. Nothing
// here writes anything or calls the engine; see probe.cpp for what each block
// decides and why every sample is taken from the same place in the frame.
namespace probe {

// A (step machinery), B (presentation) and C (slot hygiene). Call once per
// frame from the frozen step-0x2C handler, on the game thread. `macro` is the
// mission macro `this`.
void StepAndPresent(void* macro);

// D (the pad path). Call every frame from the same place; logs only when the
// ON/TRG/OFF masks change, so a single press shows up as one PRESSED line and
// one RELEASED line rather than being buried.
void PadEdges();

// Clear the per-run state so a re-entered recap starts a fresh record.
void Reset();

// Frames probed since the last Reset().
int Frames();

}  // namespace probe
