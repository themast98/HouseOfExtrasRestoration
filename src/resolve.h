#pragma once
#include <stdint.h>

namespace resolve {

// Every address here is an RVA, discovered at runtime. Nothing in this project
// outside resolve.cpp may use a hardcoded address.
struct Resolved {
    bool ok = false;

    // via RTTI: CMissionMacroColosseumExtra vtable
    uintptr_t Step2CStub    = 0;   // slot 62 - patch site 1
    uintptr_t Step2DStub    = 0;   // slot 63 - patch site 2

    // via RTTI: CMissionMacroColosseum vtable
    uintptr_t RefStep2C     = 0;   // slot 62 - the handler we mirror
    uintptr_t Step2DWorking = 0;   // slot 63 - reusable working handler

    // read out of RefStep2C
    uintptr_t TransitionSetup = 0;
    uintptr_t CommitFlags     = 0;
    uintptr_t OpenScreen      = 0;
    uintptr_t SetNextStep     = 0;
    uintptr_t PostOpenNotify  = 0;
    uintptr_t GFlagMgr        = 0;
    uintptr_t GMainMgr        = 0;
    uintptr_t KFloat          = 0;

    // RTTI: CActionColosseumExtra vtable, used to type-check [mainMgr+0xBA0]
    uintptr_t ActionExtraVft  = 0;

    // Via the DLC->save-flag bridge anchor (sub_BCD251). These unlock the six
    // rows of Bob's menu that the PC build gates behind DLC entitlements it
    // never grants. Optional: if they fail to resolve, `ok` still holds and the
    // mode-completion fix installs anyway.
    uintptr_t SetFlagAlias = 0;   // sub_28A7A0(flagMgr, alias, value)
    uintptr_t DrawElement = 0;   // sub_48BC70(elem) - immediate element draw
    uintptr_t Draw2DVtableSlot = 0;  // RVA of CActionDraw2D vtable slot 7
    uintptr_t QuadCounter = 0;   // g_quadCounter - incremented once per emitted quad
    uintptr_t SetPaneHidden = 0; // sub_48B1B0(elem, paneId, hidden) - flag bit 17
    uintptr_t PanePosReg = 0;    // sub_48AF80(elem, paneId, out, 1) - register position
    uintptr_t PaneEval   = 0;    // sub_48A780(elem) - evaluate, filling registered buffers
    uintptr_t FlagGet     = 0;   // sub_BF60F0(flagMgr, group, bit, 1) -> int
    uintptr_t FlagSetRaw  = 0;   // sub_BF8990(flagMgr, group, bit, value)  committed bank
    uintptr_t FlagSetEdge = 0;   // sub_BF8B30(flagMgr, group, bit, value)  pending list
    uintptr_t IsDlcOwned   = 0;   // sub_CB5970(ignored, bit) -> bool
    uintptr_t GDlcMask     = 0;   // the 64-byte ownership bitset IsDlcOwned reads
    bool      unlockOk     = false;

    // The mission manager global, via the MISSION_TICK anchor. Lets the watcher
    // report the running macro and its step for ANY mode, including classes we
    // never patch - so a mode that hangs is diagnosable from a single session
    // even though our own handler never runs for it.
    uintptr_t GMissionMgr  = 0;
    bool      missionDiagOk = false;

    // Layout introspection, via the NETRANK_BIND anchor. Diagnostics only - it
    // answers why a layout can be live, animating and draw-gated open yet put
    // no pixels on screen. Optional; `ok` does not depend on it.
    // sub_4843E0(layout) is `return layout->pages != NULL` - it is NOT an
    // is-loading check. Named for what it does, after the first name misread the
    // live log: pages!=NULL is the healthy state, not a stalled one.
    uintptr_t LayoutHasPages  = 0;   // sub_4843E0(layout)
    uintptr_t LayoutPageReady = 0;   // sub_484510(layout, page) -> nonzero when ready
    uintptr_t LayoutGetPane   = 0;   // sub_484220(layout, index) -> pane
    uintptr_t GLayoutRes      = 0;   // per-layout resource table, stride 0x70
    uintptr_t GLayoutTexPar   = 0;   // per-layout texture-archive handle table
    uintptr_t GLayoutNames    = 0;   // POINTER to the per-slot name table
    bool      layoutDiagOk    = false;

    // The network-ranking panel: pjs_net_ranking is the layout the deleted
    // src/ranking code drove, and it still ships. The sibling widget that draws
    // the minigame version of the same panel is intact, so these are the pieces
    // needed to build it ourselves. Optional; `ok` does not depend on them.
    uintptr_t HeapPush          = 0;   // PushHeap(heapId, 0)
    uintptr_t HeapAlloc         = 0;   // operator new(size)
    uintptr_t HeapPop           = 0;   // PopHeap()
    uintptr_t LayoutLoad        = 0;   // LoadLayout(obj, name, noTextures)
    uintptr_t NetRankCtor       = 0;   // zeroes the 0x28 panel
    uintptr_t NetRankBind       = 0;   // binds panes once the csb has loaded
    uintptr_t NetRankShow       = 0;   // Show(group, on)   [UNUSED - see netrank.cpp]
    uintptr_t NetRankDraw       = 0;   // draws the stored int as %d  [UNUSED]
    uintptr_t BuildPages        = 0;   // sub_484510(layout, kind), one-shot
    uintptr_t GetPage           = 0;   // sub_484220(layout, i), unbounded index
    uintptr_t SetSuppress       = 0;   // sub_48AC40(elem, v): elem[0xBC] = v
    uintptr_t HeapFree          = 0;   // sub_3C64D0(ptr), null-safe
    uintptr_t LayoutRelease     = 0;   // sub_483EB0(layout): unlinks elements too
    uintptr_t GLayoutSlotCount  = 0;   // authoritative slot bound
    bool      netRankOk         = false;

    // The texture-residency gate, and the pieces needed to satisfy it.
    //
    // sub_486920 picks a texture id from pane+0x4C (falling back to RS+0x1C),
    // stores it into the draw command at +0x40, and calls IsTexResident. On a
    // false result it jumps to its epilogue - AFTER sub_4889C0 has already
    // transformed all four corners of the quad. So the panel's geometry was
    // being built and then thrown away, which is exactly why enlarging its
    // sprites to full screen still lit no pixels.
    //
    // pane+0x4C is only ever written by sub_48C050 from elem+0x4E, and that
    // write is skipped while elem+0x4E is 0xFFFF - the element constructor's
    // default. Owning screens fill it in; we never did. sub_48B9D0 is the
    // one-instruction setter they use.
    //
    // Optional: `ok` and `netRankOk` do not depend on these. Without them the
    // panel behaves exactly as it did before, so a game update that breaks
    // these patterns costs the artwork, not the mod.
    uintptr_t PageCtor      = 0;   // sub_484260 - anchor only
    uintptr_t SharedTexB    = 0;   // sub_481520() -> int, a shared texture id
    uintptr_t SharedTexA    = 0;   // sub_481540() -> int, the sibling stub
    uintptr_t SetTexHandle  = 0;   // sub_48B9D0(elem, id): word[elem+0x4E] = id
    uintptr_t IsTexResident = 0;   // sub_102D750(id) -> 0/1, pure read
    bool      texGateOk     = false;

    // The engine's immediate-mode layout text API, anchored on the minigame
    // ranking widget's draw (sub_85DEC0), which is the clearest worked example
    // of it. The panel's three dynamic strings - board name, latest, best - are
    // not in the atlases (those hold only fixed labels and artwork), so they
    // have to be drawn. This API needs no font object constructed: TextBind
    // takes a PAGE RECORD, exactly what GetPage returns.
    //
    //     TextBind(page, mode, 0);
    //     *(float*)(GTextCtx + 0xD7C) = size;
    //     TextPrintf(GTextCtx, "%d", value);
    //
    // Optional; nothing else depends on these.
    uintptr_t TextBind   = 0;   // sub_4848C0(page, mode, useColor) - 924 callers
    uintptr_t TextPrintf = 0;   // sub_670BD0(ctx, fmt, ...) - varargs
    uintptr_t GTextCtx   = 0;   // the text context the two above share
    bool      textOk     = false;

    // pattern: sub_3A46D0(screen, variant, holdFrames) - the result-variant
    // setter the deleted src/ranking code called. Without it screen 220
    // constructs but its draw gate at +0x1C0 stays 0 and it renders nothing.
    uintptr_t SetResultVariant = 0;
    uintptr_t SetAnimVariant = 0;  // sub_48A520(elem, variant, flags)
    uintptr_t TagHelpDraw    = 0;  // CActionSurvivalTagHelp::Draw - the Controls panel
    uintptr_t ColosExtraVft  = 0;  // CMissionMacroColosseumExtra - Battle King / Fastest Killer
    uintptr_t AdvEscapeVft   = 0;  // CMissionMacroAdventureEscape - Speed King
    uintptr_t AdvSurviveVft  = 0;  // CMissionMacroAdventureSurvive - the tag modes
    uintptr_t SndPlay        = 0;  // SndPlay(key, int* outHandle, flags, category) -> handle
    uintptr_t SndStop        = 0;  // SndStop(handle, fadeFrames)
    uintptr_t TalkMatch      = 0;  // sub_BA7AA0(ctx, u16 out[4], talkBlock, p4) - DiagTalkMatch
    uintptr_t ColoScene      = 0;  // sub_3927E0(stageMgr) - coliseum scene-flag function - AmebaFloor
    uintptr_t FontStyleOpen  = 0;  // sub_6708E0(ctx, style) - load a face into bank 1
    uintptr_t FontStyleClose = 0;  // sub_66F200(ctx)        - release it again
};

// Resolves once and caches. Logs every address next to its expected value so a
// game update shows up as a visible mismatch rather than a silent miss.
const Resolved& Get();

// Poll until SteamStub has decrypted .text (the anchor becomes findable).
// .rdata - and therefore RTTI - is readable immediately, but .text is not.
bool WaitForText(int timeoutMs);

}  // namespace resolve
