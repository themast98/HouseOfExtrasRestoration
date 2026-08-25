#pragma once

namespace hoe {

// Absolute path to `leaf` in the directory holding the game exe.
//
// One implementation on purpose. This was hand-rolled separately in config.cpp,
// records.cpp and log.cpp; the copies drifted (one used an unbounded strcpy,
// one used the working directory instead of the exe directory), which is what
// three copies of the same fiddly code tends to produce.
//
// Returns a pointer to a cached static buffer, or nullptr if the path could not
// be built - callers must handle nullptr rather than writing to a stray path.
const char* ExeRelative(const char* leaf);

}  // namespace hoe
