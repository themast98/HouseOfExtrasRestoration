#pragma once

namespace netrank {

// Rebuilds the House of Extras end-of-mode panel on the engine's own
// pjs_net_ranking layout - the one the deleted src/ranking code drew.
//
// PS3 footage from an emulator with no PSN connection shows the panel as:
//
//        NETWORK RANKING
//   -Battle King Ranking (Kiryu)-
//   Score / Latest Record  0 / Best Record  0
//
// so it is a small LOCAL panel - your latest and your best - and the network
// only ever supplied the two numbers. Everything visual survives on PC: the
// title, the labels and the artwork are baked into pjs_net_ranking.par, which
// ships inside data/2d/cse_*.par. Only three short dynamic strings were lost.
//
// The sibling widget that draws the MINIGAME version of the same panel is
// intact, so we reuse it and simply point the load at pjs_net_ranking rather
// than the pjs_mg_net_ranking name its own loader hardcodes.
//
// Every call here must happen on the game thread.

// Allocates and loads the panel. Returns false if it could not be created;
// the caller should then just finish the step rather than wait on nothing.
bool Open();

// True once the layout has finished loading and the panes are bound. Call
// every frame until it returns true, then Show().
bool Ready();

// Binds panes and reveals the panel. Safe to call once Ready() is true.
void Show(int latest, int best);

// Per-frame draw. Does nothing until Show() has run.
void Draw();

// Tears the panel down and releases the layout.
void Close();

// True while a panel exists (between Open and Close).
bool IsOpen();

// True once the panel has been abandoned as unusable (no free layout slot,
// or the csb never published any pages). The caller should stop waiting and
// finish the step; retrying would only re-run the same broken path.
bool Failed();

}  // namespace netrank
