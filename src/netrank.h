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

// Force the score/time kind from CActionColosseumExtra+0x1B8.
void SetTimeMode(bool on);

// Tell the panel whether this run set a new best, which gates the banner.
void SetNewRecord(bool on);

// Per-frame draw. Does nothing until Show() has run.
void Draw();

// Tears the panel down and releases the layout.
void Close();

// True while a panel exists (between Open and Close).
bool IsOpen();

// Tune one of the three dynamic text slots (0 = board name, 1 = latest,
// 2 = best). Which layout page and bind mode each string belongs on was the
// deleted owner's knowledge, so it is found on screen and then baked in as the
// default. Returns false for a bad slot index.
bool SetTextSlot(int slot, int page, int mode, bool on);

// Report the current slot table into the log.
void LogTextSlots();

// Override the board-name string (the mode/character line under the title).
void SetBoardName(const char* s);

// Debug: make Close() a no-op so the panel survives the recap finishing and
// stays on screen into whatever loads next. Diagnostic only - it deliberately
// leaks a layout slot and leaves elements linked in the global render list.
void SetKeepOpen(bool on);

// True once the panel has been abandoned as unusable (no free layout slot,
// or the csb never published any pages). The caller should stop waiting and
// finish the step; retrying would only re-run the same broken path.
bool Failed();

// Stamp the 2D sort key (elem+0x48) on every page element. The element ctor
// leaves it 0 and only an owning screen ever writes a real value, so our orphan
// layout is pinned to the bottom of the 2D stack. Diagnostic + possible fix.
void SetDrawPriority(int value);

// Bias elem+0x4C so our quads sort BELOW the fullscreen black quad.
void SetSortBias(int value);

// Draw our unsuppressed page elements immediately, exactly as CActionItem does
// for the dpad HUD. Returns how many elements were drawn.
int DrawDirect();

// Swap CActionDraw2D's vtable slot 7 so our draw runs inside the engine's own 2D
// pass rather than from the macro-update phase. Reversible; Close() reverts.
bool InstallDrawHook();
void RemoveDrawHook();

// Log the texture-residency numbers for the panel's atlases.
void LogTextureGate();

// Enumerate a page's panes, marking those TextBind can target (flag 0x40).
void LogPanes(int page);

// Hide/show one pane by id (sub_48B1B0, flag bit 17).
bool HidePane(int page, unsigned short paneId, bool hidden);

// Raise or lower the X/O Close prompt (page 16).
bool ShowClosePrompt(bool on);

// Live-override the record digits' glyph fields; -1 means use the INI.
void SetNumberFont(int bank, int metric, int width, int alpha, int cellW);

// Dump the three per-bank glyph tables held inside the text context.
void LogFontTables();

// Emit the panel's strings inside the render pass (before the flush).
void DrawTextsNow();

// Draw Latest/Best with the shared page-12 pane, parked on the page-0 anchors.
void DrawNumbers();

// Unsuppress+unpause one page, or every page an enabled text slot uses.
bool ShowPage(int page, bool on);
void ShowTextPages();

// The engine's emitted-quad counter (0 if unresolved).
unsigned QuadsEmitted();

// Open/close/publish from the console, serviced on the game thread by the
// 2D-pass hook. Lets the panel be tested anywhere, with no arena run.
void RequestOpen();
void RequestClose();
void RequestPublish();

// Walk the engine's global element list and log each entry. Addresses are passed
// in rather than baked, so this stays a console-driven diagnostic.
void LogElementList(unsigned long long headRva, unsigned long long countRva);

}  // namespace netrank
