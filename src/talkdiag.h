#pragma once

// Diagnostics for Bob's missing post-mode line (DiagTalkMatch). Detours the
// stage-pac talk matcher and LOGS what it evaluates; never changes its answer.
namespace talkdiag {

// Installs the detour. Returns false (and installs nothing) if the matcher was
// not resolved or its prologue is not the 16 bytes the trampoline expects.
bool Install();

}  // namespace talkdiag
