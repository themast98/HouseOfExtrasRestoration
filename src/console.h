#pragma once

namespace console {

// Polls HouseOfExtras.cmd (next to the .asi in the mod folder) and writes HouseOfExtras.reply.
//
// MUST be called from the game thread. Engine calls that touch the layout and
// resource systems are not thread-safe, and the whole point of this channel is
// to run them where the engine itself would.
//
// Intended use: freeze the recap on screen, then drive experiments from outside
// the process - read state, poke a field, call an engine function - and look at
// the result in a screenshot, without needing a fresh play session per attempt.
//
// Every address is validated with VirtualQuery before it is touched, so a typo
// reports an error instead of killing the game.
// `allowMutation` is false when called from the background thread: pokes and
// engine calls are only safe on the game thread, so they are refused there with
// an explanatory reply rather than silently doing something dangerous. Reads
// work from either, which means the channel can be verified before anyone goes
// to the trouble of getting the game to the recap.
void Pump(bool allowMutation);

// True once a `resume` command has been received, so the recap stops freezing.
bool ResumeRequested();

}  // namespace console
