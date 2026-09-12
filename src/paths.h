#pragma once

namespace hoe {

// Absolute path to `leaf` in the directory holding HouseOfExtras.asi itself.
//
// Since 1.0.0 the .asi ships inside the mod folder (Parless loads any .asi that
// Shin Ryu Mod Manager lists for a mod), and every file the mod creates - ini,
// log, records, console cmd/reply - lives next to it. Nothing is written to the
// game root, so disabling or deleting the mod folder leaves no trace.
//
// One implementation on purpose. This was hand-rolled separately in config.cpp,
// records.cpp and log.cpp; the copies drifted (one used an unbounded strcpy,
// one used the working directory instead of the exe directory), which is what
// three copies of the same fiddly code tends to produce.
//
// Returns a pointer to a cached static buffer, or nullptr if the path could not
// be built - callers must handle nullptr rather than writing to a stray path.
const char* ModRelative(const char* leaf);

}  // namespace hoe
