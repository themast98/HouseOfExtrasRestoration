#include <windows.h>
#include "config.h"
#include "console.h"
#include "netrank.h"
#include "game.h"
#include "resolve.h"
#include "offsets.h"
#include "patch.h"
#include "log.h"

#include <string.h>
#include <stdio.h>    // snprintf, for the font-table dump

namespace netrank {
namespace {

// ---------------------------------------------------------------------------
// WHY THIS DOES NOT USE THE MINIGAME WIDGET
//
// The first two attempts drove NETRANK_BIND (sub_85E330) on this layout and
// crashed the game both times. It was never a null dereference: that function
// loops over ALL of a layout's pages and stores each page pointer at
// [panel + 8 + i*8], into a panel allocated with 0x28 bytes.
// pjs_mg_net_ranking, which it was written for, has 3 pages and fits.
// pjs_net_ranking has 17, so it writes 0x90 bytes into a 0x28 block - about a
// hundred bytes of heap corruption, faulting later and elsewhere. No amount of
// argument guarding could have helped; the callee itself was wrong for us.
//
// WHAT ACTUALLY DRAWS A LAYOUT
//
// Nothing needs to own it. Drawing is driven by a global intrusive list of
// layout ELEMENTS, walked once per frame by the render pass. Every element the
// page builder creates appends itself to that list, and is drawn when:
//     elem[0x2b] selects the 2D pass  - 0 by construction on this build path
//     elem[0x2c] bit0 is set          - the builder sets it
//     elem[0xBC] == 0                 - "not suppressed"; the builder sets 1
// So after a successful build the entire job is to clear elem[0xBC] on the
// pages we want visible. There is no per-frame draw call to make.
// ---------------------------------------------------------------------------

constexpr int kLayoutSize = 0x128;
constexpr int kHeapUi     = 10;

// LoadLayout scans the caller's name 16 bytes at a time before finding the NUL,
// so it gets a padded static buffer rather than a bare string literal.
alignas(16) char kLayoutName[0x110] = "pjs_net_ranking";

// ~10s at 60fps for the csb to stream in.
constexpr int kLoadTimeoutFrames = 600;

void* s_layout = nullptr;
int   s_slot   = -1;
int   s_pages  = 0;
bool  s_built  = false;
bool  s_shown  = false;
bool  s_failed = false;
int   s_frames = 0;
bool  s_texLogged = false;
int   s_texFrames = 0;
bool  s_texFixed  = false;   // the texture binding has settled, one way or another
bool  s_texForced = false;   // ...and it settled on the blunt whole-element override
// Debug lever (console `nr keep`). The panel is only ever built on the way
// through step 0x2C, and that step is a fade-and-teardown moment - we have
// measured a fully-formed quad of ours reaching the draw queue there (opaque,
// resident texture, on-screen coordinates, valid UVs) while the frame stays
// pure black, and no engine layout element puts a pixel up either. Holding the
// panel open across the resume lets it land somewhere 2D demonstrably
// composites, which separates "the panel is wrong" from "the moment is wrong".
bool  s_keepOpen = false;

// The texture-residency gate. sub_486920 reads a texture id from pane+0x4C,
// falls back to RS+0x1C, and calls IsTexResident with it; a false answer jumps
// to the epilogue, discarding a quad sub_4889C0 has already fully transformed.
// pane+0x4C is fed from elem+0x4E, which the element constructor leaves at
// 0xFFFF and which sub_48C050 refuses to propagate while it holds that value.
// Owning screens stamp a real id in; a bare layout like ours never did.
constexpr int      kElemTexHandle = 0x4E;   // u16
constexpr int      kResStride     = 0x70;
constexpr int      kResTexCount   = 0x3C;
constexpr int      kResTexIds     = 0x50;
constexpr int      kPaneFlags     = 0x00;
constexpr int      kPaneTexId     = 0x4C;
constexpr int      kPaneChild     = 0xA0;
constexpr int      kPaneSibling   = 0xA8;
// sub_485EF0 resolves each pane's texture ONCE and caches it: it stores the id
// at pane+0x84 and the UV rect at pane+0x88, then sets bit 0x20 in pane+0x58
// and restores those values on every later frame. The id comes from
// sub_48C3F0, which reads res+0x50[patValue >> 16] - so a pane whose first
// walk happens before the .par has published its ids caches a ZERO texture and
// keeps restoring zero for ever. That is the real defect; clearing the cache
// bit makes the pane re-resolve and cache the correct value.
constexpr int      kPaneCacheMask = 0x58;
constexpr unsigned kPaneTexCached = 0x20;
constexpr int      kPaneCachedTex = 0x84;
constexpr unsigned kPaneContainer = 0x60;   // flags & this => container, never drawn
constexpr int      kMaxTexIds     = 16;
// Frames to let the engine re-resolve before falling back. The layout
// builds in 2 frames, so 60 is generous without hanging the recap.
constexpr int      kTexFixFrames  = 60;
// sub_48C050 sign-extends elem+0x4E with cwde, so anything at or above 0x8000
// would arrive at the renderer negative. Real ids never are - the registry
// capacity is 0x4000 - but the store is ours to get wrong, so bound it here.
constexpr unsigned kMaxTexHandle  = 0x8000;
// The glyph size sub_85DEC0 sets before every printf, kept as its raw bit
// pattern so the value is verifiably the engine's own rather than a guess.
// RETIRED. This was written into ctx+0xD7C as a "text size"; that field is
// actually the glyph quads' 2D sort PRIORITY (sub_673B10 +0x674D01 does
// vcvttss2si on it and passes it as the sort key). 45056.0 == 0xB000 sits
// ABOVE the 0x3000 fullscreen black quad, so it would draw behind it.
// The value now comes from config PanelTextPrio. Glyph size is ctx+0xD94/0xD98,
// which TextBind fills from the pane's own scale - we never set it.

struct Fns {
    void  (*PushHeap)(int, int);
    void* (*Alloc)(unsigned long long);
    void  (*PopHeap)();
    void* (*LoadLayout)(void*, const char*, int);
    int   (*BuildPages)(void*, int);
    void* (*GetPage)(void*, int);
    void  (*SetSuppress)(void*, int);
    void  (*Free)(void*);
    void  (*ReleaseLayout)(void*);
    int   (*HasPages)(void*);
    const int* SlotCount;
    // Optional; all null unless resolve::Get().texGateOk.
    int   (*IsTexResident)(unsigned);
    int   (*SharedTexA)();
    int   (*SharedTexB)();
    void** PLayoutRes;                      // POINTER to the table, not the table
    // The layout text API. Null unless resolve::Get().textOk.
    void  (*TextBind)(void*, int, int);
    void  (*TextPrintf)(void*, const char*, ...);
    void*  TextCtx;
};

// Defined further down, beside the number drawing. Declared HERE, in the same
// anonymous namespace, because Close() calls the unload and is defined above the
// definition - and because a declaration at netrank scope would not match a
// definition in the anonymous namespace, which has cost this file a link error
// once already.
bool LoadRankingFont(const Fns& f);
void UnloadRankingFont(const Fns& f);
bool RankingBankReady(const void* textCtx);

bool Bind(Fns& f) {
    const auto& r = resolve::Get();
    if (!r.netRankOk || !r.LayoutHasPages) return false;
    const uintptr_t b = hoe::Base();
    f.PushHeap      = (void(*)(int, int))               (b + r.HeapPush);
    f.Alloc         = (void*(*)(unsigned long long))    (b + r.HeapAlloc);
    f.PopHeap       = (void(*)())                       (b + r.HeapPop);
    f.LoadLayout    = (void*(*)(void*, const char*, int))(b + r.LayoutLoad);
    f.BuildPages    = (int(*)(void*, int))              (b + r.BuildPages);
    f.GetPage       = (void*(*)(void*, int))            (b + r.GetPage);
    f.SetSuppress   = (void(*)(void*, int))             (b + r.SetSuppress);
    f.Free          = (void(*)(void*))                  (b + r.HeapFree);
    f.ReleaseLayout = (void(*)(void*))                  (b + r.LayoutRelease);
    f.HasPages      = (int(*)(void*))                   (b + r.LayoutHasPages);
    f.SlotCount     = (const int*)                      (b + r.GLayoutSlotCount);

    // The gate helpers are optional: without them the panel behaves exactly as
    // it did before this change, so every use below must null-check.
    f.IsTexResident = r.texGateOk ? (int(*)(unsigned))(b + r.IsTexResident) : nullptr;
    f.SharedTexB    = r.texGateOk ? (int(*)())        (b + r.SharedTexB)    : nullptr;
    f.SharedTexA    = r.SharedTexA ? (int(*)())       (b + r.SharedTexA)    : nullptr;
    f.PLayoutRes    = r.GLayoutRes ? (void**)         (b + r.GLayoutRes)    : nullptr;

    f.TextBind   = r.textOk ? (void(*)(void*, int, int))        (b + r.TextBind)   : nullptr;
    f.TextPrintf = r.textOk ? (void(*)(void*, const char*, ...))(b + r.TextPrintf) : nullptr;
    // GTextCtx is a POINTER to the context, not the context - sub_85DEC0 does
    // `mov rax, [rip+..]` and only then indexes +0xD7C. Passing the address of
    // the global instead of its value wrote four bytes into unrelated engine
    // globals at +0xD7C and handed the printf a bogus context; the game then
    // died in an unrelated list walker (sub_66EF70) on a pointer it had already
    // null-checked. Exactly the same trap as g_layoutRes at 0x1980D18, which is
    // also a pointer to its table. Re-read every frame: it is not a fixed
    // address and it can legitimately be null before the renderer is up.
    f.TextCtx    = r.textOk ? *(void**)(b + r.GTextCtx) : nullptr;
    return true;
}

// Our slot's entry in the resource table. Null unless everything checks out.
unsigned char* ResEntry(const Fns& f) {
    if (!f.PLayoutRes || s_slot < 0) return nullptr;
    auto base = (unsigned char*)*f.PLayoutRes;   // deref - this global is a POINTER
    if (!base) return nullptr;
    return base + (size_t)s_slot * kResStride;
}

bool UsableHandle(const Fns& f, unsigned id) {
    return id != 0 && id != 0xFFFFFFFFu && id < kMaxTexHandle && f.IsTexResident(id) != 0;
}

// A texture id the renderer will actually accept, or 0. Read-only: IsTexResident
// is a 0x36-byte leaf that only reads a registry, so asking it about a bad id is
// safe - writing one to the element would not be, which is why nothing reaches
// the stamp without passing through here first.
unsigned PickTextureHandle(const Fns& f) {
    if (!f.IsTexResident) return 0;

    if (unsigned char* res = ResEntry(f)) {
        const int count = *(int*)(res + kResTexCount);
        auto      ids   = *(unsigned**)(res + kResTexIds);
        if (ids && count > 0 && count <= kMaxTexIds) {
            for (int i = 0; i < count; ++i) {
                if (UsableHandle(f, ids[i])) return ids[i];
            }
        }
    }
    // Fall back to the shared containers the page constructor itself uses when
    // a layout has no atlas of its own. Both start life as -1 and only become
    // valid once their package has loaded, so they are checked like any other.
    if (f.SharedTexB) { const unsigned b = (unsigned)f.SharedTexB(); if (UsableHandle(f, b)) return b; }
    if (f.SharedTexA) { const unsigned a = (unsigned)f.SharedTexA(); if (UsableHandle(f, a)) return a; }
    return 0;
}

// ---------------------------------------------------------------------------
// The three dynamic strings.
//
// None of them are in the layout's artwork: the atlases hold "NETWORK RANKING",
// the crown and wreath, and fixed labels ("Latest", "Record", "Best Score",
// "Top 100"...), plus "NEW RECORD" and three Japanese status words. There are
// no digit glyphs at all, so the numbers have to be drawn, not selected.
//
// The engine's own immediate-mode API does that without constructing anything:
// bind the cursor to a page, set a size, printf. sub_85DEC0 is the worked
// example this is modelled on.
//
// WHICH page and mode each string wants is not knowable from the file - the
// deleted owner supplied it - so the table is tunable from the console
// (`nr text <slot> <page> <mode> <on>`) and the values found empirically get
// baked in as the defaults.
// ---------------------------------------------------------------------------
enum TextKind { kTextBoard = 0, kTextLatest = 1, kTextBest = 2,
                kTextCloseUp = 3, kTextCloseDn = 4, kTextCloseLbl = 5,
                kTextSlots = 6 };
constexpr int kClosePage = 16;   // the nine-slice Close box

struct TextSlot {
    int  page;
    int  mode;      // TextBind's 2nd argument; the widget uses 0xB and 0x1000B
    bool on;
};

// Page 0 carries the only 176x16 text node, so the board name starts there.
// The numeric slots live on pages with 16x16 text nodes - 7, 10, 12 and 16 -
// and which is which still has to be found on screen.
// All OFF by default. TextBind's mode argument is a guess copied from the
// minigame widget, and an engine call with a wrong argument is how this project
// has crashed the game three times now - most recently by handing the printf
// the address of a pointer instead of the pointer. Each slot gets switched on
// from the console during a frozen recap, watched, and only then baked in.
// The "mode" is a PANE ID, and the pane must carry flag bit 0x40 or TextBind's
// style bind (sub_48ADE0) silently registers nothing at +0x48AE64, leaving its
// output buffer zeroed - which TextBind then converts into ctx+0xD80 = colour 0,
// and +0x674CF1 copies that into all four glyph vertices. Zero colour is zero
// alpha: invisible glyphs, whatever the mode, position or priority.
//
// Every slot used to say 0x0B, and NONE of those panes has bit 0x40: page 0's
// 0x0B is a 172x88 picture, page 7 has no 0x0B at all, page 10's is a 792x1
// rule. That is why a full sweep of modes 0x00..0x1F produced byte-identical
// frames - page 0's only text pane is id 0x26 (38), outside the swept range.
//
// Text panes per page, from the shipped pjs_net_ranking.csb (flag 0x5F = text,
// 0x1F/0x3F/0x9F = picture/container):
//     page 0  -> 0x26 only        page 7  -> 1, 2, 3
//     page 10 -> 14, 15, 16, 17   page 12 -> 1
//     page 16 -> 11, 12, 13
// Use `nr panes <page>` to re-check against the LIVE tree rather than the file.
TextSlot s_text[kTextSlots] = {
    /* board    */ { 0,  0x26, true  },
    /* latest   */ { 12, 0x01, false },   // target unverified - see above
    /* best     */ { 10, 0x0E, false },   // target unverified - see above
    /* close up */ { 16, 0x0B, true  },
    /* close dn */ { 16, 0x0C, true  },
    /* close lbl*/ { 16, 0x0D, true  },
};
char s_boardName[64] = "Battle King Ranking";
bool s_timeMode = false;
// The flash is ONE event, not a loop: classifying every frame of the PS3
// capture gives a single cyan run. A wall clock rather than a frame count so
// the timing does not stretch if the frame rate is not 60.
// The count-up is FRAME driven, mirroring PS3's per-frame accumulator at
// screen+0x1E8. A wall clock was wrong in kind, not just in timing: the value
// on screen has to advance in step with the frames actually drawn, or the roll
// desyncs from what the player sees whenever a frame is skipped.
int    s_animFrame  = 0;     // frames since the numbers first drew
double s_rollAcc    = 0.0;   // PS3's screen+0x1E8
bool   s_newRecord  = false; // did this run beat the stored best?
bool   s_bannerUp   = false;
int    s_rollSeHandle = -1;  // voice handle of the roll-up loop while it plays
int    s_bannerAt   = -1;    // anim frame the full banner was raised on; -1 = not yet.
                             // Lets SetNewRecord(true) arrive AFTER the roll has
                             // settled and still show the banner - the chase board
                             // learns the verdict from the engine up to seconds late.
unsigned char s_panelAlpha = 0xFF;  // the panel's fade alpha, from slot 0

// Live overrides for the glyph fields, set by `nr font`. -1 = use the INI.
// These exist because identifying the right font bank is a search over a
// 3-value space that config alone would charge one game restart per probe.
int s_ovFont = -1, s_ovMetric = -1, s_ovWidth = -1, s_ovAlpha = -1;
int s_ovCellW = -1;

// PS3's sub_697008 does not hardcode a title: it copies a 16-row table
// {modeId, flagId, rankIndex, type} from 0x1095F6C and picks the row whose mode
// matches AND whose FLAG IS SET, then uses rankIndex to choose the string and
// type to choose Score vs Time. The flags are NETWORK_RANKING (group 199)
// bits 2..9 - the very bits our own extras-reset clears after a run, and the
// ones the log shows live during one (199:5 = Battle King, Kiryu).
//
// So select exactly the way the original does, rather than hardcoding. Order
// matters and matches PS3: read the flags BEFORE anything clears them. Our
// reset only fires once the macro is destroyed, well after the panel is built.
struct BoardRow { short bit; const char* name; bool timeMode; };
const BoardRow kBoards[] = {
    // Row 0 of PS3's board table: mission 304, flag 199:1, kind 1 = SCORE.
    // NOT the same mode as "Survival Tag SP" (flag 332:6), which has no board
    // at all and keeps its own result screen - the two were conflated for a
    // long time. Single shared board, no character suffix.
    { 1, "Endless Survival Tag Ranking",    false },
    { 2, "Battle King Ranking (Akiyama)",    false },
    { 3, "Battle King Ranking (Saejima)",    false },
    { 4, "Battle King Ranking (Tanimura)",   false },
    { 5, "Battle King Ranking (Kiryu)",      false },
    { 6, "Fastest Killer Ranking (Akiyama)", true  },
    { 7, "Fastest Killer Ranking (Saejima)", true  },
    { 8, "Fastest Killer Ranking (Tanimura)",true  },
    { 9, "Fastest Killer Ranking (Kiryu)",   true  },
    // Row 9 of PS3's board table: board 301, kind 2 = TIME, flag bit 10. Read
    // off a capture of the real thing - the subtitle is "-Speed King Ranking-"
    // with NO character suffix, because unlike Battle King and Fastest Killer
    // this is a single board rather than one per character - all four character
    // rows in Bob's menu set the SAME flag 199:10, so they share one record.
    //
    // The panel does not appear for it yet. CORRECTION to an earlier claim here:
    // its recap is NOT a gutted step pair. CMissionMacroAdventureEscape's slots
    // 62/63 do hold `ret 0`, but those are entries from the shared base-class
    // no-op POOL (+0xB52600 / +0xB52980), not deleted overrides - ColosseumExtra's
    // real gutted stubs are different functions (+0xB5C3C0 / +0xB60940), and the
    // same pool hands step 0x2B to three classes at once. Nothing in Escape's
    // step graph ever sets step 0x2C/0x2D, so those steps are never entered.
    //
    // The real hook is VTABLE SLOT 88 (+0xB56860), which Sega DELETED outright
    // rather than stubbed; MSVC then folded the now-identical function into the
    // base class, so eight classes share it. Patch the SLOT (the qword at RVA
    // 0x13752E8), never the function.
    { 10, "Speed King Ranking",              true  },
};
int  s_latest = 0;
int  s_best   = 0;

// Walk page 0's pane tree, applying `fn` to every pane. Depth- and
// breadth-bounded; the tree is 40 nodes, so the caps are slack.
template <typename F>
int ForEachPane(void* root, F fn) {
    void* stack[64];
    int   top = 0, seen = 0;
    if (root) stack[top++] = root;
    while (top > 0 && seen < 256) {
        auto p = (unsigned char*)stack[--top];
        fn(p);
        ++seen;
        void* sib = *(void**)(p + kPaneSibling);
        void* ch  = *(void**)(p + kPaneChild);
        if (sib && top < 64) stack[top++] = sib;
        if (ch  && top < 64) stack[top++] = ch;
    }
    return seen;
}

// Drop every pane's cached texture so sub_485EF0 resolves it again, this time
// with the .par's ids published. Also clears our own override, because the
// override is a blunt instrument: it forces ONE atlas onto every pane, and this
// layout ships three (256x256, 512x64, 128x64). The PAT channel packs
// (textureIndex << 16) | rectIndex, so panes belonging to the second or third
// sheet sampled the wrong one and drew garbage - visibly, "Score" and "Time"
// on top of each other.
int InvalidateTextureCache(void* elem) {
    void* root = *(void**)((unsigned char*)elem + 0x30);
    return ForEachPane(root, [](unsigned char* p) {
        *(unsigned*)(p + kPaneCacheMask) &= ~kPaneTexCached;
        *(unsigned*)(p + kPaneTexId)      = 0;
    });
}

// Copy each pane's RESOLVED atlas into the field the renderer actually reads.
//
// THE BUG THIS FIXES, read straight out of sub_486920:
//     +0x486D79  mov  eax, [pane+0x4C]      ; the texture id
//     +0x486D7E  jne  use_it                ; non-zero -> use it
//     +0x486D80  mov  eax, [RS+0x1C]        ; else a GLOBAL default
//     +0x486D88  call IsTexResident
//     +0x486D8F  je   skip_emit             ; not resident -> no geometry
//     ...        call +0x482960             ; the emit we never reached
//
// InvalidateTextureCache zeroes pane+0x4C to force sub_485EF0 to resolve again.
// The engine duly re-resolves - but into the CACHE at pane+0x84, and nothing
// ever writes the answer back to +0x4C. So the renderer read 0, fell through to
// the global default, found it not resident at the recap, and jumped past the
// emit with the geometry already transformed. In the palace that same default
// happens to be resident, which is the entire reason the identical layout drew
// there and not here - and why every element-side theory came up empty.
//
// We verified success on pane+0x84 too, which is why the fallback path never
// ran and this stayed invisible.
int PublishResolvedTextures(const Fns& f, void* elem) {
    void* root = *(void**)((unsigned char*)elem + 0x30);
    int published = 0;
    ForEachPane(root, [&](unsigned char* p) {
        const unsigned live = *(unsigned*)(p + kPaneTexId);
        if (live) return;                            // already has one
        const unsigned cached = *(unsigned*)(p + kPaneCachedTex);
        if (!cached || !f.IsTexResident(cached)) return;
        *(unsigned*)(p + kPaneTexId) = cached;
        ++published;
    });
    return published;
}

// First pane in the tree that can actually emit geometry. The two panes sampled
// in the earlier investigation were both containers (flags 0x3F has bit 0x20),
// which sub_486920 skips before either the alpha test or the texture gate - so
// measuring them proved nothing. Depth- and breadth-bounded.
void* FirstLeafPane(void* root) {
    void* stack[64];
    int   top = 0;
    if (root) stack[top++] = root;
    for (int guard = 0; top > 0 && guard < 256; ++guard) {
        auto p = (unsigned char*)stack[--top];
        if ((*(unsigned*)(p + kPaneFlags) & kPaneContainer) == 0) return p;
        void* sib = *(void**)(p + kPaneSibling);
        void* ch  = *(void**)(p + kPaneChild);
        if (sib && top < 64) stack[top++] = sib;
        if (ch  && top < 64) stack[top++] = ch;
    }
    return nullptr;
}

// page -> element, both checked. Returns null if either is missing.
void* ElementOf(const Fns& f, int i) {
    void* page = f.GetPage(s_layout, i);      // returns null if pages is null;
    if (!page) return nullptr;                // never bounds-checks i, hence s_pages
    return *(void**)((unsigned char*)page + off::C_PAGE_ELEM_FIELD);
}

}  // namespace

void PickBoardName();
bool ShowPage(int page, bool on);
void ShowTextPages();

bool IsOpen() { return s_layout != nullptr; }
bool Failed() { return s_failed; }

bool Open() {
    Fns f;
    if (!Bind(f)) { hoe::Log("netrank: pieces did not resolve, cannot build"); return false; }
    if (s_layout) return true;

    f.PushHeap(kHeapUi, 0);
    void* lay = f.Alloc(kLayoutSize);
    if (lay) {
        memset(lay, 0, kLayoutSize);
        // LoadLayout writes a vtable at [obj] and the name at [obj+0xC], so it
        // must be given real memory; both engine callers null-check the
        // allocation exactly like this before handing it over.
        f.LoadLayout(lay, kLayoutName, 0);
    }
    f.PopHeap();

    if (!lay) { hoe::Log("netrank: allocation failed"); return false; }

    // The slot must be inside the engine's own bound. LoadLayout leaves -1 when
    // every slot is busy, and the page builder indexes the resource table with
    // it unchecked - a -1 reads 0x70 bytes BEFORE the table.
    const int slot  = *(int*)((unsigned char*)lay + off::C_LAYOUT_RES_INDEX_FIELD);
    const int limit = f.SlotCount ? *f.SlotCount : 0;
    if (slot < 0 || limit <= 0 || slot >= limit) {
        hoe::Log("netrank: layout slot %d outside 0..%d - releasing and giving up",
                 slot, limit - 1);
        // Release even on this path: a load already claimed the slot's flags, so
        // simply dropping the object would burn one of the few slots for good.
        f.ReleaseLayout(lay);
        f.Free(lay);
        return false;
    }

    s_layout = lay;
    s_slot   = slot;
    s_pages  = 0;
    s_built = s_shown = s_failed = false;
    s_animFrame = 0; s_rollAcc = 0.0; s_bannerUp = false; s_bannerAt = -1;   // rearm the animation
    if (s_rollSeHandle >= 0) { game::StopSE(s_rollSeHandle); s_rollSeHandle = -1; }   // never leave the loop running
    s_frames = 0;
    s_texLogged = false;
    s_texFrames = 0;
    s_texFixed = s_texForced = false;
    hoe::Log("netrank: layout=%p slot=%d ('%s')", lay, slot, kLayoutName);
    return true;
}

bool Ready() {
    if (!s_layout || s_failed) return false;
    if (s_built) return true;

    Fns f;
    if (!Bind(f)) return false;
    ++s_frames;

    // The builder returns 0 for as many frames as the async csb request needs,
    // and 1 exactly once, on the frame it actually builds. Falling through on a
    // 0 return is the single most likely way to crash here, because the page
    // array is still null and GetPage would hand back null.
    if (!f.HasPages(s_layout)) {
        if (f.BuildPages(s_layout, 0) == 0) {
            if (s_frames % 60 == 0) {
                hoe::Log("netrank: waiting for the csb, %d frames", s_frames);
            }
            if (s_frames > kLoadTimeoutFrames) {
                hoe::Log("netrank: csb never arrived - giving up");
                s_failed = true;
            }
            return false;
        }
    }
    // Re-test rather than trusting the return: the builder can report success
    // in paths where the page array is still not usable.
    if (!f.HasPages(s_layout)) return false;

    InstallDrawHook();
    s_pages = game::LayoutPageCount(s_layout);
    if (s_pages <= 0) {
        hoe::Log("netrank: built but reports %d pages - giving up", s_pages);
        s_failed = true;
        return false;
    }

    // Verify EVERY page and element before touching any of them. The pane pool
    // is a fixed free list, and this layout needs far more panes than the
    // minigame one, so an exhausted pool leaving a null element is a live path.
    for (int i = 0; i < s_pages; ++i) {
        if (!ElementOf(f, i)) {
            hoe::Log("netrank: page %d of %d has no element - giving up", i, s_pages);
            s_failed = true;
            return false;
        }
    }

    s_built = true;
    hoe::Log("netrank: built after %d frames, %d pages, all elements present",
             s_frames, s_pages);
    return true;
}

// elem+0x2c bit 0x02 is the page builder's "created suppressed" marker, and the
// animation advance treats it as "paused" (sub_48BC70+0x1F8). Clear it so the
// element's clock runs, and give the clock a positive start: the advance is also
// skipped while the time is exactly 0, so a page left at 0.0 never begins.
// Both writes are values the engine itself produces - it clears 0x08 and sets
// 0x10 on this field on the very next frame - so this only puts the element into
// the state an engine-owned one is already in.
constexpr unsigned kElemPaused = 0x02;

void Unpause(void* elem) {
    auto p = (unsigned char*)elem;
    unsigned& flags = *(unsigned*)(p + 0x2C);
    float&    time  = *(float*)(p + 0x38);
    const unsigned before = flags;
    flags &= ~kElemPaused;
    if (time <= 0.0f) time = 1.0f;
    hoe::Log("netrank: unpaused elem %p (+0x2c %#x -> %#x, clock started)",
             elem, before, flags);
}

// Phase 2 diagnostic: dump our pane root in the same shape as the reference
// captured from elements that ARE drawing (refs/drawing_pane_reference.json).
// Read-only. A drawing pane has the low 6 bits set at +0x000, non-null pointers
// at +0x008/+0x010/+0x098, a running time at +0x018, 0x7B at +0x058 and 1.0f at
// +0x060/+0x068 - so whichever of those we differ on is the answer.
void LogPaneRoot(void* elem) {
    if (!elem) return;
    void* root = *(void**)((unsigned char*)elem + 0x30);
    hoe::Log("  pane: elem=%p root=%p", elem, root);
    if (!root) { hoe::Log("  pane: ROOT IS NULL - nothing can be drawn"); return; }
    auto p = (unsigned char*)root;
    // Bounded, fixed-size read of the same window the reference covers.
    for (int o = 0; o < 0xB0; o += 0x20) {
        hoe::Log("  pane: +0x%03X  %016llX %016llX %016llX %016llX", o,
                 (unsigned long long)*(uint64_t*)(p + o),
                 (unsigned long long)*(uint64_t*)(p + o + 8),
                 (unsigned long long)*(uint64_t*)(p + o + 0x10),
                 (unsigned long long)*(uint64_t*)(p + o + 0x18));
    }
    const unsigned flags = *(unsigned*)(p + 0x00);
    hoe::Log("  pane: flags=0x%X (drawing panes have the low 6 bits set) "
             "ptrs +08=%p +10=%p +98=%p  scale/alpha +60=%g +68=%g",
             flags, *(void**)(p + 8), *(void**)(p + 0x10), *(void**)(p + 0x98),
             *(float*)(p + 0x60), *(float*)(p + 0x68));
}

void Show(int latest, int best) {
    if (!s_layout || !s_built || s_shown) return;
    Fns f;
    if (!Bind(f)) return;

    // The builder creates every page suppressed AND paused, and those are the
    // same decision: sub_484260 takes one branch when its 4th argument is 0 and
    // that branch both calls SetSuppress(elem, 1) and ORs 0x02 into elem+0x2c.
    // Clearing the suppress flag alone is only half of "show it", because the
    // forward animation advance in sub_48BC70 opens with
    //     test al, 2      ; al = elem+0x2c
    //     jne  skip       ; bit 0x02 set -> the clock never moves
    // so the element sits on frame 0 of its animation for ever. Measured live:
    // elem+0x38 stayed exactly 0.0 across sixteen minutes of a frozen recap.
    // Every engine element that is visibly drawing has bit 0x02 clear
    // (pjs_btl 0x451, pjs_kill_counter 0x51, pjs_life 0x51); ours had 0x4B.
    void* elem = ElementOf(f, 0);
    if (!elem) { hoe::Log("netrank: page 0 element vanished"); return; }
    f.SetSuppress(elem, 0);
    Unpause(elem);
    LogPaneRoot(elem);

    s_latest = latest;
    s_best   = best;
    s_shown  = true;
    hoe::Log("netrank: page 0 unsuppressed (latest=%d best=%d)", latest, best);
}

// One block of numbers that says which operand was wrong, logged on the first
// frame and then rarely until a handle is found. Read-only.
void LogTexGate(const Fns& f, unsigned handle, void* elem) {
    unsigned char* res = ResEntry(f);
    if (!res) { hoe::Log("  texgate: no resource entry for slot %d", s_slot); return; }

    const int count = *(int*)(res + kResTexCount);
    auto      ids   = *(unsigned**)(res + kResTexIds);
    hoe::Log("  texgate: res flags=0x%X texCount=%d ids=%p",
             *(unsigned*)res, count, (void*)ids);
    if (ids && count > 0 && count <= kMaxTexIds) {
        for (int i = 0; i < count; ++i) {
            hoe::Log("  texgate:   ids[%d]=0x%X resident=%d", i, ids[i],
                     f.IsTexResident ? f.IsTexResident(ids[i]) : -1);
        }
    } else {
        hoe::Log("  texgate:   id array unusable - nothing was ever published here");
    }
    if (f.SharedTexB) hoe::Log("  texgate: sharedB=0x%X resident=%d", (unsigned)f.SharedTexB(),
                               f.IsTexResident((unsigned)f.SharedTexB()));
    if (f.SharedTexA) hoe::Log("  texgate: sharedA=0x%X resident=%d", (unsigned)f.SharedTexA(),
                               f.IsTexResident((unsigned)f.SharedTexA()));

    if (elem) {
        void* leaf = FirstLeafPane(*(void**)((unsigned char*)elem + 0x30));
        if (leaf) {
            auto lp = (unsigned char*)leaf;
            hoe::Log("  texgate: leaf pane %p flags=0x%X +0x4C=0x%X (must latch to the handle)",
                     leaf, *(unsigned*)(lp + kPaneFlags), *(unsigned*)(lp + kPaneTexId));
        } else {
            hoe::Log("  texgate: no leaf pane found - every pane is a container?");
        }
    }
    hoe::Log("  texgate: chose handle=0x%X%s", handle, handle ? "" : "  <- NOTHING VALID TO STAMP");
}

// Immediate mode: this has to run every frame, from the game thread, inside the
// frame. Bind positions the cursor on the page; printf emits the glyphs.
void DrawTexts(const Fns& f) {
    if (!f.TextBind || !f.TextPrintf || !f.TextCtx) return;

    for (int i = 0; i < kTextSlots; ++i) {
        const TextSlot& t = s_text[i];
        if (!t.on) continue;
        // GetPage never bounds-checks, so the page index is checked here.
        if (t.page < 0 || t.page >= s_pages) continue;
        void* page = f.GetPage(s_layout, t.page);
        if (!page) continue;

        f.TextBind(page, t.mode, 0);

        // The board slot binds a page-0 pane that IS animated, so the alpha byte
        // of the colour its bind establishes is the panel's current fade level -
        // it was seen ramping 0x2A, 0x55 over consecutive frames. PS3 ORs
        // exactly this byte into the numbers' colour at 0x698814, which is what
        // fades them in with the rest of the panel instead of popping them in at
        // full strength. Capture it here; DrawNumbers applies it.
        if (i == kTextBoard) {
            s_panelAlpha = *((unsigned char*)f.TextCtx + off::C_TEXT_CTX_ALPHA_BYTE);
        }

        // Read back what TextBind actually established. Colour 0 means the style
        // bind was skipped, i.e. the pane id is wrong (no flag 0x40). A sane
        // colour with a sensible position and scale means the bind landed and
        // any remaining invisibility is priority, not targeting.
        if (config::Get().DiagTextBind) {
            auto cx = (unsigned char*)f.TextCtx;
            static int s_shown[kTextSlots] = {0, 0, 0};
            if (s_shown[i] < 2) {
                ++s_shown[i];
                hoe::Log("  textbind slot %d page %d pane 0x%X -> colour=0x%08X "
                         "pitch=0x%X glyph=%dx%d align=0x%X pos=(%d,%d) "
                         "scale=(%.2f,%.2f) prio=%.0f%s",
                         i, t.page, t.mode,
                         *(unsigned*)(cx + 0xD80), *(unsigned*)(cx + 0xD9C),
                         *(int*)(cx + off::C_TEXT_CTX_GLYPH_H),
                         *(int*)(cx + off::C_TEXT_CTX_GLYPH_W),
                         *(unsigned*)(cx + 0xD78),
                         *(int*)(cx + 0xD70), *(int*)(cx + 0xD74),
                         *(float*)(cx + 0xD94), *(float*)(cx + 0xD98),
                         *(float*)(cx + off::C_TEXT_CTX_PRIO_FIELD),
                         *(unsigned*)(cx + 0xD80) ? "" :
                         "   <== COLOUR 0: WRONG PANE, style bind skipped");
            }
        }
        // ctx+0xD7C is NOT a glyph size. sub_673B10 does
        //     vcvttss2si r9, [ctx+0xD7C]
        // and passes r9 as the PRIORITY down sub_263940 -> sub_10322F0 ->
        // sub_107B4F0, where it becomes node+0x0C = (prio << 16) | flags - the
        // same descending sort key the layout quads use. The glyph SIZE lives in
        // ctx+0xD94/0xD98 and TextBind fills it from the pane's own scale.
        //
        // The engine widget sub_85DEC0 writes 45056.0 (0xB000), which is ABOVE
        // the 0x3000 fullscreen black quad and would therefore be drawn behind
        // it. Ours has to sit below the black quad AND below the panel's own
        // ~0x0F6D so the glyphs land on top of their own panel.
        *(float*)((unsigned char*)f.TextCtx + off::C_TEXT_CTX_PRIO_FIELD) =
            (float)config::Get().PanelTextPrio;
        // Bracket the printf with the 2D display list's own node count. This is
        // the instrument that cracked the panel, and it is the only way to
        // settle where the glyphs go: an async console read CANNOT see it,
        // because the append and the flush both happen inside one frame (a
        // between-frames read shows the pre-walk 39 every time).
        //
        //   delta > 0  -> glyph quads ARE reaching the list; the problem is
        //                 downstream (sort key, colour, position, or the batch
        //                 being reset before the flush)
        //   delta == 0 -> TextPrintf produced nothing at all, and the answer is
        //                 inside sub_670BD0 / sub_673B10
        unsigned before = 0, after = 0;
        auto L = *(unsigned char**)(hoe::Base() + 0x34E0B90);
        const bool haveList = (L != nullptr);
        if (haveList) before = *(unsigned*)(L + 0x18);

        switch (i) {
        // PS3 wraps the board name in dashes: -Battle King Ranking (Kiryu)-
        case kTextBoard:  f.TextPrintf(f.TextCtx, "-%s-", s_boardName); break;
        case kTextLatest: f.TextPrintf(f.TextCtx, "%d", s_latest);    break;
        case kTextBest:   f.TextPrintf(f.TextCtx, "%d", s_best);      break;
        // The button glyphs are font escapes, the same form the game's own
        // menus use; the label is plain text.
        case kTextCloseUp:  f.TextPrintf(f.TextCtx, "%s", "<Sign:D>"); break;
        case kTextCloseDn:  f.TextPrintf(f.TextCtx, "%s", "<Sign:C>"); break;
        case kTextCloseLbl: f.TextPrintf(f.TextCtx, "%s", "Close");    break;
        }

        if (haveList && config::Get().DiagTextBind) {
            after = *(unsigned*)(L + 0x18);
            static int s_said[kTextSlots] = {0, 0, 0, 0, 0, 0};
            if (s_said[i] < 2) {
                ++s_said[i];
                hoe::Log("  textquads slot %d: list %u -> %u (%+d node(s))%s",
                         i, before, after, (int)after - (int)before,
                         after > before ? ""
                                        : "   <== NO GLYPH QUADS PRODUCED");
            }
        }
    }
}

void Draw() {
    // The engine's render pass draws the panel off the global element list, so
    // there is no per-frame draw call to make. What there IS to do is repair the
    // texture binding, in two stages.
    //
    // PREFERRED: drop each pane's cached texture (pane+0x58 bit 0x20) so
    // sub_485EF0 resolves it again now that res+0x50 holds real ids. That gives
    // every pane the atlas its OWN PAT channel asks for - the value packs
    // (textureIndex << 16) | rectIndex and this layout has three sheets.
    //
    // FALLBACK: if re-resolving still leaves a pane with no texture, force one
    // id onto the whole element via elem+0x4E. That makes the panel visible but
    // wrong for any pane not on sheet 0, so it is a last resort, not the plan.
    if (!s_layout || !s_built || s_failed) return;

    Fns f;
    if (!Bind(f) || !f.IsTexResident) return;

    // Re-read the element through GetPage rather than caching it: Close() can
    // run between frames, and a stale pointer here would write into freed heap.
    void* elem = ElementOf(f, 0);
    if (!elem) return;

    if (!s_texFixed) {
        ++s_texFrames;
        const int n = InvalidateTextureCache(elem);
        // Give the engine a frame to re-resolve, then check a leaf actually
        // cached something. Re-invalidating every frame would never let it
        // settle, so this only runs until it takes or the budget runs out.
        void* leaf = FirstLeafPane(*(void**)((unsigned char*)elem + 0x30));
        const unsigned cached =
            leaf ? *(unsigned*)((unsigned char*)leaf + kPaneCachedTex) : 0;
        if (cached && f.IsTexResident(cached)) {
            s_texFixed = true;
            // The cache is populated - now publish it where sub_486920 looks.
            const int pub = PublishResolvedTextures(f, elem);
            // The panel is built; bias it below the fullscreen black quad.
            SetSortBias(config::Get().PanelSortBias);

            PickBoardName();

            // NOT ShowTextPages(). Unsuppressing pages 10 and 12 drew a SECOND
            // copy of the panel frame offset ~40px down and washed the whole
            // thing out: those pages are not bare digit slots, they carry
            // alternate copies of the panel artwork. Page visibility is not the
            // right lever for the numbers, and the pane targets for latest/best
            // were guessed from a csb parse rather than taken from what PS3
            // actually binds - which has never been traced. Leave the number
            // slots off until it has been.
            ShowPage(kClosePage, true);

            // Score vs Time. PS3 hides one of the two coincident page-0 labels
            // according to the mode's "type"; Battle King is a score mode
            // (type 1), so hide 0x22 ("Time", 46px) and leave 0x23 ("Score").
            if (config::Get().PanelHideTimeLabel) {
                // A score mode hides "Time" (0x22); a time mode hides "Score"
                // (0x23). PS3 drives this from the same table row.
                HidePane(0, s_timeMode ? 0x23 : 0x22, true);
            }
            // And raise the X/O Close prompt (page 16), suppress-only - no
            // SetAnimVariant, which would rebuild the pane tree.
            if (config::Get().PanelClosePrompt) {
                ShowClosePrompt(true);
            }
            hoe::Log("netrank: texture cache re-resolved across %d panes; leaf "
                     "cache holds 0x%X; published to pane+0x%X on %d pane(s)",
                     n, cached, kPaneTexId, pub);
            // Every other explanation is now dead: occlusion, priority, element
            // state, frame phase, and whether the 2D pass runs are all measured
            // and cleared, yet a draw from INSIDE the pass still emits nothing.
            // The one gate left sits inside the emit itself - sub_486920 asks
            // IsTexResident and jumps past the emit on a false answer - and it
            // depends on GLOBAL state, not on the element. Which is exactly the
            // shape of "identical element, draws in the palace, not at the
            // recap". So say the residency numbers out loud, every run.
            LogTexGate(f, cached, elem);
        } else if (s_texFrames >= kTexFixFrames) {
            const unsigned handle = PickTextureHandle(f);
            if (handle) {
                *(unsigned short*)((unsigned char*)elem + kElemTexHandle) =
                    (unsigned short)handle;
                s_texForced = true;
            }
            s_texFixed = true;
            hoe::Log("netrank: re-resolve did not take after %d frames - forcing "
                     "0x%X on the whole element (artwork on other sheets will be "
                     "wrong)", s_texFrames, handle);
            LogTexGate(f, handle, elem);
        }
        return;
    }

    // Keep it published: the engine can re-resolve a pane at any time, and each
    // time it does the answer lands in the cache again, not in +0x4C.
    if (const int pub = PublishResolvedTextures(f, elem)) {
        static int s_lastPub = -1;
        if (pub != s_lastPub) {
            s_lastPub = pub;
            hoe::Log("netrank: re-published texture id to %d pane(s)", pub);
        }
    }

    // The forced path only: sub_48BC70 resets elem+0x4E to 0xFFFF after every
    // walk, so it has to be re-stamped. The re-resolved path needs nothing here.
    if (s_texForced) {
        const unsigned handle = PickTextureHandle(f);
        if (handle) {
            *(unsigned short*)((unsigned char*)elem + kElemTexHandle) =
                (unsigned short)handle;
        }
    }

    // NOT here any more. MEASURED: DrawTexts called from this path starts with
    // the display list already at 91 nodes - i.e. AFTER the engine's element
    // walk, and therefore after the flush at the end of sub_2B7BB0. The glyph
    // quads were genuinely produced (slot 5 "Close" added exactly 5 nodes) and
    // then thrown away by the reset. Same wrong-phase error as the panel draw,
    // and this time the node counts show it rather than an inference.
    // EmitOurPanel() calls DrawTextsNow() before the original pass instead.
}

void SetKeepOpen(bool on) { s_keepOpen = on; }

bool SetTextSlot(int slot, int page, int mode, bool on) {
    if (slot < 0 || slot >= kTextSlots) return false;
    s_text[slot].page = page;
    s_text[slot].mode = mode;
    s_text[slot].on   = on;
    hoe::Log("netrank: text slot %d -> page %d mode %#x %s",
             slot, page, mode, on ? "ON" : "off");
    return true;
}

void LogTextSlots() {
    static const char* kNames[kTextSlots] = { "board", "latest", "best" };
    for (int i = 0; i < kTextSlots; ++i) {
        hoe::Log("netrank: slot %d %-6s page=%d mode=%#x %s", i, kNames[i],
                 s_text[i].page, s_text[i].mode, s_text[i].on ? "ON" : "off");
    }
    hoe::Log("netrank: board=\"%s\" latest=%d best=%d (pages available: %d)",
             s_boardName, s_latest, s_best, s_pages);
}

void SetBoardName(const char* s) {
    if (!s) return;
    // Bounded: the console hands us an arbitrary line.
    size_t n = 0;
    while (s[n] && n < sizeof(s_boardName) - 1) ++n;
    memcpy(s_boardName, s, n);
    s_boardName[n] = 0;
    hoe::Log("netrank: board name -> \"%s\"", s_boardName);
}

void Close() {
    if (!s_layout) return;
    if (s_keepOpen) {
        hoe::Log("netrank: Close() suppressed (nr keep) - layout %p stays live in "
                 "slot %d and its elements stay in the render list", s_layout, s_slot);
        return;
    }
    Fns f;
    if (!Bind(f)) { s_layout = nullptr; return; }

    // ReleaseLayout is not optional and not merely a free: it destroys the page
    // array through its vector-deleting destructor, and that is what UNLINKS
    // each element from the global render list. Skip it and the renderer keeps
    // walking freed memory a frame later.
    // Balance the font load before the layout goes: the loader overwrites the
    // bank's globals without releasing, so an unmatched open leaks them.
    UnloadRankingFont(f);

    f.ReleaseLayout(s_layout);
    f.Free(s_layout);
    hoe::Log("netrank: released layout %p (slot %d)", s_layout, s_slot);

    s_layout = nullptr;
    s_slot = -1;
    s_pages = 0;
    s_built = s_shown = s_failed = false;
    s_animFrame = 0; s_rollAcc = 0.0; s_bannerUp = false; s_bannerAt = -1;   // rearm the animation
    if (s_rollSeHandle >= 0) { game::StopSE(s_rollSeHandle); s_rollSeHandle = -1; }   // never leave the loop running
    s_frames = 0;
    s_texLogged = false;
    s_texFrames = 0;
    s_texFixed = s_texForced = false;

    // The draw hook deliberately STAYS INSTALLED.
    //
    // This used to call RemoveDrawHook() here, and that single line is why no
    // House of Extras mode could be restarted after the first recap of a
    // session. game::ObserveModeEnd() - which clears a finished mode's start
    // flag so Bob will offer it again - runs from HoE_Draw2DPass and nowhere
    // else (it cannot run on the background heartbeat: writing save flags off
    // the game thread is exactly what this mod forbids). Tearing the hook down
    // on dismissal killed the observer for the rest of the session, so the
    // SECOND mode of any session was never unlocked. It looked like a Speed
    // King bug only because Speed King happened to be played second.
    //
    // It also removed a subtler landmine: s_latched in ObserveModeEnd is a
    // function-static that survived the gap, so when the next Open() reinstalled
    // the hook, the first observation could compare against a macro that had
    // been freed while the hook was down and fire a reset from a stale latch.
    //
    // Leaving it installed is not a new state: the logged session ran hooked and
    // idle for 216 seconds before the first panel existed, with the pass
    // confirmed live. RemoveDrawHook is kept for a genuine shutdown path.
}


// --- 2D draw priority -------------------------------------------------------
//
// elem+0x48 is the ONLY key of sub_486F70, the stable ascending signed sort that
// sub_488E00 runs immediately before its sequential DrawElement loop: lowest key
// draws first (behind), highest draws last (on top). The element constructor
// sub_4893C0 zeroes it (+0x489419 stores a zeroed rsi across +0x44/+0x48), and
// every owning SCREEN stamps a real value right beside its SetSuppress calls -
// the engine's own band runs from -10 for backgrounds (sub_97E020) up to 0x10
// for topmost windows (sub_8839A0).
//
// Our layout has no owning screen, so all 17 page elements sit at 0, the bottom
// of the stack. That is equally true in the palace where the panel IS visible,
// so priority is not by itself the difference - but it leaves us maximally
// vulnerable to anything opaque that does carry a real priority.
// Bias our elements BELOW the fullscreen black quad so they draw on top of it.
//
// MEASURED, by walking the engine's own 2D display list at the frozen recap and
// decoding every one of its 83 nodes:
//   prio 0xBF6D..0xBF8E   our 34 quads, opaque white, correct geometry
//                         (frame at 224,232-1056,488; body 248,268-1032,456)
//   prio 0x3000           a FULLSCREEN OPAQUE BLACK QUAD, (0,0)-(1920,1080),
//                         colour FF000000, texture 0
//   prio 0x0000           the dpad HUD (layer 0x001A)
// The list is merge-sorted DESCENDING on the qword at node+0x08, whose top 16
// bits are the priority, and the draw loop walks the sorted array forwards. So
// HIGHER priority draws FIRST (backmost) and LOWER draws LAST (on top). Our
// panel was therefore painted first and the black quad painted straight over it,
// while the dpad - lowest priority in the whole list - survived on top. That is
// the entire bug, and it explains every symptom simultaneously.
//
// The per-quad priority is clamp((s16)def[+0x0E] + (u16)elem[+0x4C] -
// [0x1980D24], 0, 0xFFFF), computed at +0x486B23..+0x486B54. elem+0x4C is the
// only part we own: the element ctor sub_4893C0 writes 0xC000 there (+0x4893E9)
// and it has exactly one reader, the line above. Dropping it to 0x1000 moves us
// to ~0x0F6D..0x0F8E - below the black quad, above the dpad.
//
// WRITE IT AS A u16. elem+0x4E is a SEPARATE field (0xFFFF, written at
// +0x4893F0) which sub_48C050 sign-extends into pane+0x4C, the texture id tested
// at +0x486D79. A dword write here would blank every pane's texture. The pair is
// why dumps show 0xFFFFC000.
void SetSortBias(int value) {
    if (!s_layout) { hoe::Log("netrank: bias - no layout open"); return; }
    Fns f;
    if (!Bind(f)) { hoe::Log("netrank: bias - functions unresolved"); return; }

    int done = 0, skipped = 0;
    for (int i = 0; i < s_pages; ++i) {
        void* e = ElementOf(f, i);
        if (!e) continue;
        auto bias = (unsigned short*)((unsigned char*)e + off::C_ELEM_SORT_KEY_FIELD);
        const unsigned short old = *bias;
        // Only touch the ctor default. Anything else means something we do not
        // know about owns this field, and we want to hear about it rather than
        // clobber it.
        if (old != 0xC000 && old != (unsigned short)value) {
            if (skipped < 3) {
                hoe::Log("  bias: page %d elem %p holds 0x%04X, not the ctor "
                         "default 0xC000 - leaving it alone", i, e, old);
            }
            ++skipped;
            continue;
        }
        *bias = (unsigned short)value;
        if (i < 3) hoe::Log("  bias: page %d elem %p 0x%04X -> 0x%04X", i, e, old, *bias);
        ++done;
    }
    hoe::Log("netrank: sort bias = 0x%04X on %d/%d page elements (%d skipped) - "
             "below the 0x3000 fullscreen black quad, above the dpad",
             (unsigned)(value & 0xFFFF), done, s_pages, skipped);
}

void SetDrawPriority(int value) {
    if (!s_layout) { hoe::Log("netrank: prio - no layout open"); return; }
    Fns f;
    if (!Bind(f)) { hoe::Log("netrank: prio - functions unresolved"); return; }

    int done = 0;
    for (int i = 0; i < s_pages; ++i) {
        void* e = ElementOf(f, i);
        if (!e) continue;
        // MUST be u16. elem+0x4E is a SEPARATE field holding the texture handle
        // that our own texture fix installs, and sub_48C050 sign-extends it into
        // pane+0x4C, the id sub_486920 tests at +0x486D79. A 32-bit write here
        // silently destroys the texture fix and the panel goes blank again -
        // which would look like a brand new mystery. SetSortBias got this right;
        // this path did not.
        auto prio = (unsigned short*)((unsigned char*)e + off::C_ELEM_SORT_KEY_FIELD);
        const unsigned short old = *prio;
        *prio = (unsigned short)value;
        if (i < 4 || old != 0) {
            hoe::Log("  prio: page %2d elem %p  %d -> %d", i, e, old, *prio);
        }
        ++done;
    }
    hoe::Log("netrank: draw priority = %d on %d/%d page elements",
             value, done, s_pages);
}

// --- global element list ----------------------------------------------------
//
// Names whatever is drawing over us. Addresses come in from the console rather
// than being baked, so this stays a diagnostic and not a second source of truth.
void LogElementList(unsigned long long headRva, unsigned long long countRva) {
    const uintptr_t base = hoe::Base();
    auto head = *(unsigned char**)(base + headRva);
    const unsigned count = *(unsigned*)(base + countRva);
    hoe::Log("elements: head=%p live=%u", head, count);
    if (count > 4096) {
        hoe::Log("elements: count %u looks wrong - refusing to walk", count);
        return;
    }

    // Snapshot our own page elements so the dump can mark them.
    unsigned char* ours[64];
    int ourN = 0;
    if (s_layout) {
        Fns f;
        if (Bind(f)) {
            for (int i = 0; i < s_pages && ourN < 64; ++i) {
                if (void* e2 = ElementOf(f, i)) ours[ourN++] = (unsigned char*)e2;
            }
        }
    }

    auto e = head;
    for (unsigned i = 0; i < count && e; ++i) {
        const unsigned pass  = *(unsigned char*)(e + off::C_ELEM_PASS_FIELD);
        const unsigned flags = *(unsigned*)(e + off::C_ELEM_FLAGS_FIELD);
        const int prio       = *(int*)(e + off::C_ELEM_SORT_KEY_FIELD);
        const unsigned supp  = *(unsigned*)(e + off::C_ELEM_SUPPRESS_FIELD);
        auto res             = *(unsigned char**)(e + 0x10);
        bool mine = false;
        for (int k = 0; k < ourN; ++k) {
            if (ours[k] == e) { mine = true; break; }
        }
        hoe::Log("  elem %p res=%p pass=%u flags=0x%X prio=%d supp=%u%s",
                 e, res, pass, flags, prio, supp, mine ? "   <== OURS" : "");
        e = *(unsigned char**)(e + off::C_ELEM_NEXT_FIELD);
    }
}


// --- draw ourselves, the way the HUD does -----------------------------------
//
// THE ASYMMETRY THIS EXISTS TO EXPLOIT. The dpad HUD is visible at the recap
// while our panel is not, and measurement has ruled out every difference in the
// elements themselves: our 17 elements sit in the engine's global list with
// pass=0, drawable flags, page 0 unsuppressed, and (after a test poke) the
// highest priority in the game - 236 live elements, not one with a non-zero
// priority - and still nothing appeared. So it was never occlusion and never
// priority.
//
// The one asymmetry left is WHO DRIVES THE DRAW. CActionItem (action 73, the
// dpad, layout pjs_btl) does not use the sorted global pass at all: its
// vfunc+0x38 (sub_2FBAF0) calls GetPage then DrawElement itself, every frame,
// from its own category-22 draw node. Our layout is an orphan - no owning
// screen, no draw node - so it depends entirely on something else running the
// sorted pass, which evidently happens in the palace (where the panel IS
// visible, confirmed repeatedly) and not at the recap.
//
// So stop depending on it. We already have a per-frame hook at the recap; call
// DrawElement ourselves from there.
//
// HONEST CAVEAT: our step handler runs in the macro-update phase, not the render
// phase. If the display list has not been opened yet, or has already been
// flushed by the time we run, these calls land nowhere and this experiment
// simply shows nothing. That is a real possibility and it is exactly what the
// test is for - a null result here is informative, not a bug.
// The engine's own emitted-quad counter, or 0 if it could not be resolved.
unsigned QuadsEmitted() {
    const auto& r = resolve::Get();
    if (!r.QuadCounter) return 0;
    return *(unsigned*)(hoe::Base() + r.QuadCounter);
}

int DrawDirect() {
    if (!s_layout) return 0;
    const auto& r = resolve::Get();
    if (!r.DrawElement) return 0;
    Fns f;
    if (!Bind(f)) return 0;

    auto Draw = (void(*)(void*))(hoe::Base() + r.DrawElement);

    int drawn = 0;
    for (int i = 0; i < s_pages; ++i) {
        void* e = ElementOf(f, i);
        if (!e) continue;
        // Skip suppressed pages - the engine's own walk does the same, and
        // drawing all 17 would stack every page on top of each other.
        if (*(unsigned*)((unsigned char*)e + off::C_ELEM_SUPPRESS_FIELD)) continue;
        Draw(e);
        ++drawn;
    }
    return drawn;
}


// --- draw inside the engine's own 2D pass -----------------------------------
//
// WHY. DrawDirect() from the step handler was MEASURED drawing 1 element every
// frame at the recap with nothing appearing on screen. The calls are real; they
// just happen in the macro-update phase, outside the display list's lifetime.
// The dpad HUD gets on screen because CActionItem::vfunc+0x38 is invoked from a
// category-22 DRAW NODE, i.e. during the render pass. We have no draw node, so
// we borrow one: swap CActionDraw2D's vtable slot 7 (its vfunc+0x38, the 2D
// layout pass itself, +0x2B7BB0), call the original, then draw our page.
//
// A vtable swap rather than a code detour on purpose: we keep the original
// pointer and call through it, so there is no trampoline to get wrong on a hot
// per-frame function. Installed only while the panel is open.
//
// This also answers a question I could not settle by reading memory: if the hook
// never logs, CActionDraw2D's pass does not run at the recap at all, and the
// panel can never appear this way no matter what we draw.
namespace {

using Draw2DFn = void (*)(void*);
Draw2DFn  s_origDraw2D = nullptr;
// Console requests, serviced inside the hook because the console itself runs on
// a background thread and must never touch engine state directly.
volatile long s_reqOpen  = 0;
volatile long s_reqClose = 0;
volatile long s_reqPublish = 0;
bool s_freeRun = false;      // panel opened outside a recap
uintptr_t s_hookSlotRva = 0;
bool      s_hookRan = false;
volatile long s_inHook = 0;

}  // namespace


// --- read-only render probes ------------------------------------------------
//
// WHY THESE EXIST. The counter at 0x1980D30 does NOT prove a quad reached the
// display list: sub_482960, sub_10322F0 and sub_107B4F0 all return void, and
// sub_107B4F0 has three silent-drop paths (header overflow +0x107B508, null
// arena +0x107B51E, vertex-block overflow +0x107B5A5) after which sub_486920
// still executes `inc [0x1980D30]` unconditionally. "34 quads emitted" only
// ever meant "the residency gate was passed 34 times". Two experiments were
// built on that number believing it meant more.
//
// So measure the things that actually decide visibility:
//   A  the fade block - is a full-alpha black fade up?
//   B  the display list - did our append survive, or was it silently dropped?
//   C  our own nodes' SORT KEYS - the list is sorted DESCENDING on the qword at
//      node+0x08 with priority in bits 48..63, so LOWER priority draws LAST i.e.
//      on top. The fade quad sits at 0x3000; the element ctor gives elements
//      0xC000. If our nodes really are above 0x3000, the fade is drawn over us.
//
// Everything here is a plain read. Nothing writes engine state.
namespace {

struct ListSnap { void* arena; void* head; unsigned cap, cursor, count; bool ok; };

ListSnap SnapList() {
    ListSnap s{};
    auto pL = (unsigned char**)(hoe::Base() + 0x34E0B90);
    auto L = *pL;
    if (!L) return s;
    s.arena  = *(void**)(L + 0x00);
    s.head   = *(void**)(L + 0x08);
    s.cap    = *(unsigned*)(L + 0x10);
    s.cursor = *(unsigned*)(L + 0x14);
    s.count  = *(unsigned*)(L + 0x18);
    s.ok = true;
    return s;
}

void LogFadeBlock() {
    const uintptr_t B = hoe::Base();
    const unsigned char mode = *(unsigned char*)(B + 0x198C624);
    hoe::Log("probe A fade: mode=%u rgb=%02X%02X%02X alpha=%.3f target=%.3f "
             "step=%.4f frames=%d prio=0x%04X%s",
             mode,
             *(unsigned char*)(B + 0x198C627), *(unsigned char*)(B + 0x198C626),
             *(unsigned char*)(B + 0x198C625),
             *(float*)(B + 0x198C630), *(float*)(B + 0x198C638),
             *(float*)(B + 0x198C634), *(int*)(B + 0x198C628),
             *(unsigned short*)(B + 0x198C63C),
             mode ? "" : "   <== NO FADE QUAD AT ALL - fade theory dead");
}

// Walk the nodes appended between two snapshots and report their sort keys.
// Link, verified both directions: sub_107B4F0 +0x107B5C5 writes
// rel = (u32)oldHead - (u32)(node+4); sub_107B870 +0x107B881 reads
// next = (node+4) + rel.
void LogOurNodes(const ListSnap& before, const ListSnap& after) {
    if (!after.ok || !after.head) { hoe::Log("probe C: no list head"); return; }
    auto n = (unsigned char*)after.head;
    int i = 0;
    unsigned lo = 0xFFFFu, hi = 0;
    while (n && (void*)n != before.head && i < 64) {
        const unsigned long long key = *(unsigned long long*)(n + 0x08);
        const unsigned prio = (unsigned)(key >> 48);
        const unsigned tex  = *(unsigned short*)(n + 0x08);
        if (prio < lo) lo = prio;
        if (prio > hi) hi = prio;
        if (i < 4) {
            hoe::Log("  probe C node[%d] %p key=%016llX tex=%u prio=0x%04X",
                     i, n, key, tex ? tex - 1 : 0, prio);
        }
        const int rel = *(int*)(n + 0x04);
        if (!rel) break;
        n = (n + 4) + rel;
        ++i;
    }
    const unsigned short fadePrio = *(unsigned short*)(hoe::Base() + 0x198C63C);
    hoe::Log("probe C: %d node(s) ours, prio range 0x%04X..0x%04X, fade prio 0x%04X -> %s",
             i, lo, hi, fadePrio,
             (i && lo > fadePrio) ? "WE SORT BEHIND THE FADE (it draws on top of us)"
                                  : "we are not behind the fade");
}

}  // namespace

void DrainConsoleRequests();
void EmitOurPanel();
void DrawTextsNow();
void DrawNumbers();
// Defined at the bottom, at THIS scope - declaring it inside the anonymous
// namespace makes the call resolve to a symbol that is never defined.
// Banner states, declared HERE because DrawNumbers uses them well above the
// definitions. Defining an enum at the bottom and using it at the top is the
// same ordering mistake that has already cost this file two build breaks.
enum class Banner { Hidden, Full, Parked };
void SetRecordBanner(Banner state);
void ShowRecordBanner(bool on);
void PickBoardName();

extern "C" __attribute__((ms_abi)) void HoE_Draw2DPass(void* self) {
    // ORDER IS EVERYTHING HERE, and getting it wrong cost a run.
    //
    // sub_2B7BB0 is the layout walk AND THE FLUSH - the flush is straight-line
    // code a few bytes after the walk. Calling the original first and emitting
    // afterwards puts our quads into the buffer AFTER it has already been
    // flushed, so they are discarded before they can reach the backbuffer.
    // MEASURED: 34 quads emitted every frame for 60 seconds, screen black.
    //
    // That is also exactly why the dpad HUD is visible - CActionItem emits from
    // its own draw node DURING the walk, ahead of the flush. So do what it does:
    // emit first, then let the engine's pass run and flush our quads with its own.
    // Startup trace: a handful of lines over the first ~3 seconds, which is the
    // window a hard crash once fell into with no log to show for it. Bounded and
    // sparse, so it costs nothing after the first few seconds.
    if (config::Get().DiagStartup) {
        static int s_startupFrame = 0;
        const int f = ++s_startupFrame;
        if (f <= 3 || f == 10 || f == 20 || f == 40 || f == 60 ||
            f == 90 || f == 120 || f == 180) {
            game::LogStartupState(f);
        }
    }

    DrainConsoleRequests();

    // Detect a finished mode and clear its start flag. This runs HERE, and not
    // on the 2s unlock heartbeat, for two reasons:
    //
    //   SPEED. Per frame instead of every 2 seconds, so a mode unlocks before
    //   the player can walk back to Bob. The recap path only ever felt instant
    //   because ArmExtrasReset spins up a 250ms sampler - which exists solely
    //   while DiagMissionTrace is on and expires after two minutes, so even that
    //   was not dependable.
    //
    //   THREAD SAFETY, which matters more. Clearing a flag calls the game's own
    //   FlagSetRaw/FlagSetEdge, and this mod's own rule is that mutations belong
    //   on the game thread - the console enforces exactly that with
    //   NeedGameThread(). Firing the reset from the background heartbeat broke
    //   that rule; this is the only callback that runs on the game thread every
    //   frame, so it is where the write belongs.
    //
    // The check itself is two pointer reads and a compare, and early-outs when
    // the macro slot has not changed.
    game::ObserveModeEnd();

    // Reports the drain's own clock. See game.cpp: this reads the LOGIC frame
    // counter, not this hook's rate - measuring the hook is what made the previous
    // version report 32Hz dips on a steady 60Hz machine.
    game::ReportChaseDrainClock();

    // Speed King intro: trace the Escape macro and re-arm the title card per run.
    game::TraceEscapeMacro();
    game::TraceStartCaption();
    game::ObserveCaptionSeen();
    game::TraceExtrasLaunch();
    game::ReplayChaseIntro();

    const bool probing = s_layout != nullptr;
    ListSnap a0{}, a1{}, a2{};
    if (probing) a0 = SnapList();

    EmitOurPanel();

    if (probing) a1 = SnapList();

    if (s_origDraw2D) s_origDraw2D(self);

    if (probing) {
        a2 = SnapList();
        static int s_probeFrames = 0;
        if (s_probeFrames < 3 || (s_probeFrames % 120) == 0) {
            LogFadeBlock();
            hoe::Log("probe B list: arena=%p cap=0x%X | before count=%u cursor=0x%X"
                     " | after-ours count=%u cursor=0x%X (+%d nodes, +0x%X bytes)"
                     " | after-engine count=%u cursor=0x%X",
                     a0.arena, a0.cap, a0.count, a0.cursor, a1.count, a1.cursor,
                     (int)a1.count - (int)a0.count, a1.cursor - a0.cursor,
                     a2.count, a2.cursor);
            if (a1.count == a0.count) {
                hoe::Log("probe B: OUR APPEND WAS SILENTLY DROPPED - one of the three "
                         "no-return guards in sub_107B4F0 fired. The 0x1980D30 counter "
                         "has been lying.");
            }
            LogOurNodes(a0, a1);

            // Probe 1: the walker's gate from sub_488E00 +0x489156, reproduced
            // exactly, on each of our page elements. If WOULD_DRAW is false the
            // answer is one field and 0x48AC40 fixes it; if true, the engine is
            // walking us and the problem is downstream of the walk.
            {
                Fns pf;
                if (Bind(pf)) {
                    for (int i = 0; i < s_pages && i < 3; ++i) {
                        auto e = (unsigned char*)ElementOf(pf, i);
                        if (!e) continue;
                        const unsigned pass  = *(unsigned char*)(e + 0x2B);
                        const unsigned flags = *(unsigned*)(e + 0x2C);
                        const unsigned supp  = *(unsigned*)(e + 0xBC);
                        const unsigned prio  = *(unsigned short*)(e + 0x4C);
                        const float    alpha = *(float*)(e + 0x38);
                        const bool would = (pass != 2 && pass != 3) && (flags & 1) &&
                                           (supp == 0 || (flags & 0x400));
                        hoe::Log("  probe 1 page %d %p pass=%u flags=0x%X supp=%u "
                                 "prio=0x%04X alpha=%.3f -> WOULD_DRAW=%s",
                                 i, e, pass, flags, supp, prio, alpha,
                                 would ? "true" : "FALSE");
                    }
                }
            }
        }
        ++s_probeFrames;
    }

    if (!s_hookRan) {
        s_hookRan = true;
        hoe::Log("draw2d: CActionDraw2D pass IS running at the recap - hook live");
    }

    InterlockedExchange(&s_inHook, 0);
}

// Serviced on the game thread, where engine calls are legal.
void DrainConsoleRequests() {
    // The console runs on a background thread and cannot touch engine state, so
    // this hook is also its pump. Without this call the mutation console only
    // worked inside a frozen recap.
    console::Pump(true);

    if (InterlockedCompareExchange(&s_reqOpen, 0, 1) == 1) {
        if (!s_layout) {
            s_freeRun = Open();
            hoe::Log("netrank: free-run open -> %s", s_freeRun ? "ok" : "FAILED");
        } else {
            hoe::Log("netrank: already open");
        }
    }
    if (InterlockedCompareExchange(&s_reqClose, 0, 1) == 1) {
        if (s_layout) { SetKeepOpen(false); Close(); s_freeRun = false; hoe::Log("netrank: free-run closed"); }
    }
    if (InterlockedCompareExchange(&s_reqPublish, 0, 1) == 1) {
        Fns pf;
        if (Bind(pf)) {
            if (void* e0 = ElementOf(pf, 0)) {
                hoe::Log("netrank: manual publish -> %d pane(s)",
                         PublishResolvedTextures(pf, e0));
            }
        }
    }

}

// Emit our page BEFORE the engine's walk+flush, so our quads are in the buffer
// when it flushes.
void EmitOurPanel() {
    if (InterlockedCompareExchange(&s_inHook, 1, 0) != 0) return;

    // A free-run panel has no step handler driving it, so drive it here.
    if (s_freeRun && s_layout) {
        if (Ready()) Show(0, 0);
        Draw();
    }

    // Text goes in here, before the original pass walks and flushes.
    DrawTextsNow();
    DrawNumbers();
    // OFF BY DEFAULT NOW, and the default is the point.
    //
    // Our element is already in the engine's global element list at 0x1980D70
    // with pass=0, flags=0x51, suppress=0 - which satisfies the walker's own gate
    // in sub_488E00 (+0x489156) verbatim. So the engine SHOULD be drawing it
    // without any help from us.
    //
    // Worse, helping is actively harmful: sub_488E00 is what (re)assigns the
    // per-frame scratch pointers 0x1980D50 / 0x1980D58 / 0x1980D40 / 0x1980D48
    // at +0x488E76..+0x488EA5, and 0x1980D58 IS the staging quad sub_482960
    // reads. We emit BEFORE calling the original, i.e. before that assignment,
    // so every one of our draws has been going through last frame's stale
    // scratch. The "emit before the flush" reasoning this was built on is also
    // unfounded - nobody has established where the sprite queue is reset.
    //
    // So: stop helping, and find out what the engine does on its own. The quad
    // counter is deliberately not consulted here either - it has no reader
    // anywhere in the image and only proves the residency gate was passed.
    if (s_layout && config::Get().PanelDirectDraw) {
        const int n = DrawDirect();
        static int s_last = -1;
        if (n != s_last) {
            s_last = n;
            hoe::Log("draw2d: drew %d element(s) from inside the pass "
                     "(PanelDirectDraw=1 - the engine's own walk is NOT being "
                     "trusted this run)", n);
        }
    }
    InterlockedExchange(&s_inHook, 0);
}

bool InstallDrawHook() {
    if (s_origDraw2D) return true;                 // already installed
    const auto& r = resolve::Get();
    if (!r.Draw2DVtableSlot) {
        hoe::Log("draw2d: vtable slot unresolved - hook not installed");
        return false;
    }

    s_hookSlotRva = r.Draw2DVtableSlot;
    auto slot = (uintptr_t*)(hoe::Base() + s_hookSlotRva);
    s_origDraw2D = (Draw2DFn)*slot;

    const uintptr_t ours = (uintptr_t)&HoE_Draw2DPass;
    if (!hoe::WriteBytes(s_hookSlotRva, &ours, sizeof(ours))) {
        hoe::Log("draw2d: could not write the vtable slot");
        s_origDraw2D = nullptr;
        return false;
    }
    s_hookRan = false;
    hoe::Log("draw2d: hooked vtable slot +0x%llX (orig +0x%llX -> ours %p)",
             (unsigned long long)s_hookSlotRva,
             (unsigned long long)((uintptr_t)s_origDraw2D - hoe::Base()),
             (void*)ours);
    return true;
}

void RemoveDrawHook() {
    if (!s_origDraw2D || !s_hookSlotRva) return;
    const uintptr_t orig = (uintptr_t)s_origDraw2D;
    hoe::WriteBytes(s_hookSlotRva, &orig, sizeof(orig));
    hoe::Log("draw2d: vtable slot restored%s",
             s_hookRan ? "" : " - NOTE the pass never ran while hooked");
    s_origDraw2D = nullptr;
    s_hookSlotRva = 0;
}


// Same numbers on demand, so the recap can be compared against the palace where
// the identical layout demonstrably draws.
void LogTextureGate() {
    if (!s_layout) { hoe::Log("texgate: no layout open"); return; }
    Fns f;
    if (!Bind(f)) { hoe::Log("texgate: functions unresolved"); return; }
    void* elem = ElementOf(f, 0);
    unsigned cached = 0;
    if (elem) {
        if (void* leaf = FirstLeafPane(*(void**)((unsigned char*)elem + 0x30))) {
            cached = *(unsigned*)((unsigned char*)leaf + 0x4C);
        }
    }
    LogTexGate(f, cached, elem);
}


// Console-facing requests. These only set a flag; the 2D-pass hook performs the
// work on the game thread, where engine calls are legal.
void RequestOpen()    { InterlockedExchange(&s_reqOpen, 1); }
void RequestClose()   { InterlockedExchange(&s_reqClose, 1); }
void RequestPublish() { InterlockedExchange(&s_reqPublish, 1); }


// Enumerate a page's panes and mark the ones TextBind can actually target.
//
// A pane is a legal text target iff its flag word has bit 0x40 - that is the
// gate sub_48ADE0 tests at +0x48AE64 before registering the style buffer that
// supplies colour, font, alignment and outline. The pane's ID is the value the
// BST in those three registration routines matches on:
//     id = *(u16*)( *(void**)(node + 0x10) + 8 )
// and that ID is exactly what the mod passes as "mode".
void LogPanes(int page) {
    if (!s_layout) { hoe::Log("panes: no layout open"); return; }
    Fns f;
    if (!Bind(f)) { hoe::Log("panes: functions unresolved"); return; }
    if (page < 0 || page >= s_pages) {
        hoe::Log("panes: page %d out of range (0..%d)", page, s_pages - 1);
        return;
    }
    void* e = ElementOf(f, page);
    if (!e) { hoe::Log("panes: page %d has no element", page); return; }

    int total = 0, text = 0;
    ForEachPane(*(void**)((unsigned char*)e + 0x30), [&](unsigned char* n) {
        ++total;
        const unsigned flags = *(unsigned*)n;
        auto def = *(unsigned char**)(n + 0x10);
        const unsigned id = def ? *(unsigned short*)(def + 8) : 0xFFFF;
        const bool isText = (flags & off::C_TEXT_PANE_FLAG) != 0;
        if (isText) ++text;
        if (isText || total <= 4) {
            hoe::Log("  pane page %d id=0x%X flags=0x%X %s", page, id, flags,
                     isText ? "  <== TEXT TARGET" : "");
        }
    });
    hoe::Log("panes: page %d has %d pane(s), %d usable as text targets",
             page, total, text);
}


// --- Score vs Time, and the Close prompt ------------------------------------
//
// PS3 sub_698... at 0x69854C picks between two coincident labels on PAGE 0:
//     if (type <= 1)  SetPaneHidden(page0elem, 0x22, 1)     // hide "Time"
//     else            SetPaneHidden(page0elem, 0x23, 1)     // hide "Score"
// The csb confirms the pair: pane 0x22 is 46px wide (4 glyphs = "Time") and
// 0x23 is 56px (5 glyphs = "Score"), both at the same position (0,-30). They
// are drawn on top of each other because we never hid either one - it is a
// variant selection we were not making, not a layout defect.
//
// Battle King is a score mode (PS3 type 1), so we hide 0x22 and "Score" stays.
//
// sub_48B1B0 sets flag bit 17 (0x20000) = HIDDEN: +0x48B22D clears it,
// +0x48B233 shifts the 3rd argument left 17 and ORs it in. The draw gate at
// +0x486AB8 shifts bit 17 into state+0x34 and +0x486AF8 then skips the pane.
// Guards, from its disassembly: it dereferences rcx+0x30 unconditionally (so
// the element must be non-null), the 3rd argument is used RAW (anything but
// 0/1 would set the force-draw bit 0x40000), and a nonzero high byte in the id
// becomes a sibling-hop count whose loop walks [rax+0xA8] with no null check -
// so pass the id bare.
bool HidePane(int page, unsigned short paneId, bool hidden) {
    const auto& r = resolve::Get();
    if (!r.SetPaneHidden) { hoe::Log("hidepane: unresolved"); return false; }
    if (!s_layout) return false;
    Fns f;
    if (!Bind(f)) return false;
    void* e = ElementOf(f, page);
    if (!e) { hoe::Log("hidepane: page %d has no element", page); return false; }
    if (paneId & 0xFF00) {
        hoe::Log("hidepane: refusing id 0x%X - a nonzero high byte is a hop count",
                 paneId);
        return false;
    }
    auto fn = (void(*)(void*, unsigned, unsigned))(hoe::Base() + r.SetPaneHidden);
    fn(e, paneId, hidden ? 1u : 0u);
    hoe::Log("hidepane: page %d pane 0x%X -> %s", page, paneId,
             hidden ? "HIDDEN" : "shown");
    return true;
}

// Raise the Close prompt. Page 16 is the nine-slice rounded box (panes 2..10)
// plus three text panes 0x0B / 0x0C / 0x0D, which PS3 binds as modes
// 0x10000B / 0x10000C / 0x10000D at 0x69A2D4 / 0x69A3A8 / 0x69A480 - the low
// word is the pane id, so 0x0B/0x0C/0x0D are what matters. (All three
// registrars decode the argument as skip=(id>>8)&0xFF then pane=id&0xFFFF at
// +0x48AFAD, so bits 16+ are simply ignored - the high word is NOT a page.)
//
// Stage 5a deliberately uses ONLY levers already exercised safely: clear the
// pause bit and unsuppress. It does NOT call SetAnimVariant (+0x48A520), whose
// accepted path runs a full pane-tree REBUILD via +0x4868B0 - by some way the
// most invasive thing available, and not worth it until we can see whether the
// box lands on screen without it. If it sits off-screen mid-slide, that is the
// signal to consider 5b.
bool ShowClosePrompt(bool on) {
    if (!s_layout) return false;
    Fns f;
    if (!Bind(f)) return false;
    void* g = ElementOf(f, kClosePage);
    if (!g) { hoe::Log("close: page %d has no element", kClosePage); return false; }
    if (on) {
        *(unsigned*)((unsigned char*)g + off::C_ELEM_FLAGS_FIELD) &= ~0x02u;  // unpause
    }
    f.SetSuppress(g, on ? 0 : 1);
    hoe::Log("close: prompt page %d %s", kClosePage, on ? "SHOWN" : "hidden");
    return true;
}


// Emit the panel's strings. Must run INSIDE the render pass and BEFORE the
// engine's flush, or the glyph quads are appended to a list that has already
// been drawn and is about to be reset.
void DrawTextsNow() {
    if (!s_layout || !s_built || s_failed) return;
    Fns f;
    if (!Bind(f)) return;
    DrawTexts(f);
}


// Choose the board title and the Score/Time flavour from the live save flags,
// the way sub_697008 does. Falls back to whatever is already set if no flag is
// up - which happens if something cleared them before we looked.
void PickBoardName() {
    void* fm = game::pFlagMgr ? *game::pFlagMgr : nullptr;
    if (!fm || !game::FlagGet) {
        hoe::Log("board: no flag manager - keeping '%s'", s_boardName);
        return;
    }
    for (const auto& b : kBoards) {
        if (!game::FlagGet(fm, 199, b.bit, 1)) continue;
        SetBoardName(b.name);
        s_timeMode = b.timeMode;
        hoe::Log("board: flag 199:%d set -> '%s' (%s mode)", b.bit, b.name,
                 b.timeMode ? "time" : "score");
        return;
    }
    hoe::Log("board: no NETWORK_RANKING flag 199:1..9 is set - keeping '%s'",
             s_boardName);
}


// Make a page visible. Pages 1..16 are built SUPPRESSED (elem+0xBC == 1) and
// paused (elem+0x2C bit 0x02), which is why their panes report colour
// 0x00FFFFFF - alpha 0 - when TextBind evaluates them, and why "latest" and
// "best" stayed blank while the subtitle on page 0 rendered fine. Page 16
// needed exactly this treatment before the Close box appeared.
bool ShowPage(int page, bool on) {
    if (!s_layout) return false;
    Fns f;
    if (!Bind(f)) return false;
    if (page < 0 || page >= s_pages) {
        hoe::Log("showpage: %d out of range (0..%d)", page, s_pages - 1);
        return false;
    }
    void* e = ElementOf(f, page);
    if (!e) { hoe::Log("showpage: page %d has no element", page); return false; }
    if (on) *(unsigned*)((unsigned char*)e + off::C_ELEM_FLAGS_FIELD) &= ~0x02u;
    f.SetSuppress(e, on ? 0 : 1);
    hoe::Log("showpage: page %d %s", page, on ? "shown" : "hidden");
    return true;
}

// Show every page an enabled text slot draws into. Doing it from the slot table
// rather than a hardcoded list means retargeting a slot cannot silently leave
// its page invisible again - which is the bug this function exists to end.
void ShowTextPages() {
    bool done[64] = {};
    for (int i = 0; i < kTextSlots; ++i) {
        const TextSlot& s = s_text[i];
        if (!s.on || s.page < 0 || s.page >= 64) continue;
        if (done[s.page]) continue;
        done[s.page] = true;
        ShowPage(s.page, true);
    }
}


// --- the two record numbers -------------------------------------------------
//
// PS3 draws BOTH numbers with a SINGLE pane - page 12 (0x0C) pane 0x01, a
// one-pane "big number" page - by physically re-parking that page over two
// anchor panes on page 0 and printing twice (sub_6986AC, 0x698718 and 0x698908):
//
//     read position of page-0 pane 0x24        (the Latest anchor)
//     move page 12's root there                 (sub_196FB8)
//     TextBind(page 12, pane 0x01)
//     ...override the context...
//     TextPrintf("%lld", screen+0x1F0)          -> LATEST
//     ...repeat with anchor 0x25 and screen+0x200 -> BEST
//
// We reach the same result without moving anything: TextBind writes the draw
// position into ctx+0xD70/0xD74 (+0x484A8E/+0x484A94), so we simply overwrite
// those with the anchor's position after binding. That avoids poking a page
// object whose layout we have not verified.
//
// Overriding the context AFTER TextBind is the engine's own PC idiom, not an
// invention: sub_85DEC0 does TextBind, then writes ctx+0xD7C, then TextPrintf.
// TextBind resets the context wholesale (+0x48495F), so every override must
// come after the bind and be repeated for the second number.
//
// Page 10 is NOT involved. Its panes 0x0E..0x11 are the online send/receive
// dialog lines, and page 10 carries a duplicate copy of the frame art - which
// is exactly what appeared when the page was unsuppressed.
namespace {

// --- the ranking numeral face -------------------------------------------
//
// See the module comment above DrawOneNumber for why bank 1 is empty on PC and
// what the three patch sites are. Everything here is transient: the patches are
// reverted the instant the engine's loader returns.

bool  s_fontOpen = false;      // we called the open and owe a matching close
char* s_cavePrefix = nullptr;  // "ranking/ranking_"
char* s_caveFormat = nullptr;  // "data/font_hd/%shankaku.dds"

// A rip-relative lea can only reach +/-2GB, so the replacement strings have to
// live near the image rather than inside our own DLL, which the loader is free
// to place anywhere.
void* AllocNear(uintptr_t anchorAddr, size_t n) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity
                                                      : 0x10000;
    for (uintptr_t delta = gran; delta < 0x40000000u; delta += gran) {
        for (int up = 0; up < 2; ++up) {
            const uintptr_t addr = up ? anchorAddr + delta : anchorAddr - delta;
            if (up == 0 && addr > anchorAddr) continue;      // wrapped below zero
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((void*)addr, &mbi, sizeof mbi)) continue;
            if (mbi.State != MEM_FREE) continue;
            void* got = VirtualAlloc((void*)(addr & ~(uintptr_t)(gran - 1)), n,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (got) return got;
        }
    }
    return nullptr;
}

bool EnsureCave() {
    if (s_cavePrefix) return true;
    void* page = AllocNear(hoe::Base(), 0x1000);
    if (!page) { hoe::Log("font: no free page within 2GB of the image"); return false; }
    s_cavePrefix = (char*)page;
    s_caveFormat = s_cavePrefix + 0x40;
    strcpy(s_cavePrefix, "ranking/ranking_");
    strcpy(s_caveFormat, "data/font_hd/%shankaku.dds");
    hoe::Log("font: cave at %p (prefix %p, format %p)", page, s_cavePrefix, s_caveFormat);
    return true;
}

// Is bank 1's halfwidth sheet resident? -1 means the slot was never filled.
bool RankingBankReady(const void* textCtx) {
    if (!textCtx) return false;
    return *(const int*)((const unsigned char*)textCtx
                         + off::C_TEXT_CTX_SHEET + 1 * 4) != -1;
}

bool LoadRankingFont(const Fns& f) {
    if (s_fontOpen) return true;
    const auto& r = resolve::Get();
    if (!r.FontStyleOpen || !f.TextCtx) return false;
    if (!EnsureCave()) return false;

    const uintptr_t base = hoe::Base();
    const intptr_t d1 = (intptr_t)s_cavePrefix
                      - (intptr_t)(base + off::C_FONT_PREFIX_DISP + 4);
    const intptr_t d2 = (intptr_t)s_caveFormat
                      - (intptr_t)(base + off::C_FONT_FORMAT_DISP + 4);
    if (d1 != (intptr_t)(int)d1 || d2 != (intptr_t)(int)d2) {
        hoe::Log("font: cave out of rip-relative range (%lld, %lld)",
                 (long long)d1, (long long)d2);
        return false;
    }
    const int d1i = (int)d1, d2i = (int)d2;
    const unsigned char zero = 0;

    hoe::Patch prefix, format, zen;
    if (!prefix.Apply(off::C_FONT_PREFIX_DISP, &d1i, 4, "font prefix -> ranking")) {
        return false;
    }
    if (!format.Apply(off::C_FONT_FORMAT_DISP, &d2i, 4, "font path -> font_hd")) {
        prefix.Revert(); return false;
    }
    if (!zen.Apply(off::C_FONT_ZENKAKU_IMM, &zero, 1, "skip zenkaku pages")) {
        format.Revert(); prefix.Revert(); return false;
    }

    auto open = (void (*)(void*, int))(base + r.FontStyleOpen);
    open(f.TextCtx, off::C_FONT_BANK_STYLE);

    // Straight back out. The strings and the guard are consumed synchronously
    // while building the request, so nothing else ever sees them altered.
    zen.Revert();
    format.Revert();
    prefix.Revert();

    s_fontOpen = true;
    hoe::Log("font: requested the ranking face into bank 1 (style %d)",
             (int)off::C_FONT_BANK_STYLE);
    return true;
}

void UnloadRankingFont(const Fns& f) {
    if (!s_fontOpen) return;
    const auto& r = resolve::Get();
    if (r.FontStyleClose && f.TextCtx) {
        ((void (*)(void*))(hoe::Base() + r.FontStyleClose))(f.TextCtx);
        hoe::Log("font: released bank 1");
    }
    s_fontOpen = false;
}

constexpr int   kNumberPage   = 0x0C;   // the shared one-pane big-number page
constexpr short kNumberPane   = 0x01;
constexpr short kAnchorLatest = 0x24;   // page-0 panes that position each number
constexpr short kAnchorBest   = 0x25;

// The sentinel a time board holds until it has been played. VERIFIED in the PS3
// binary rather than assumed: the record reset builds 0x157529FF at +0xB50CBC,
// and the recap compares the Best value against it at +0x698A34..+0x698A40.
constexpr int kNoTimeRecord = 359999999;   // 0x157529FF

// ...and when it matches, PS3 does NOT format it as 99:59'59"99 - it prints this
// literal instead (+0x698A48 loads 0x1070C0C -> the string at 0xEC3124, then
// calls the same printf). An earlier version of this code formatted the sentinel
// as a time, which rendered a nonsense record of 99:59'59"99.
constexpr char kNoTimeText[] = "-:--'--\"--";

// Ask the engine where an anchor pane sits. sub_48AF80 registers the output
// buffer, sub_48A780 evaluates the tree and fills it.
bool AnchorPos(const Fns& f, void* page0elem, short paneId, float* outXY) {
    const auto& r = resolve::Get();
    if (!r.PanePosReg || !r.PaneEval || !page0elem) return false;
    auto reg  = (void(*)(void*, unsigned, void*, unsigned))(hoe::Base() + r.PanePosReg);
    // evalTree is a pointer to the GAME's pane-tree evaluator sub_48A780;
    // nothing here interprets code or data.
    auto evalTree = (void(*)(void*))(hoe::Base() + r.PaneEval);
    float buf[8] = {};
    reg(page0elem, (unsigned)paneId, buf, 1);
    evalTree(page0elem);
    outXY[0] = buf[0];
    outXY[1] = buf[1];
    return true;
}

void DrawOneNumber(const Fns& f, void* page0elem, short anchor, int value,
                   bool highlight, int slotForLog) {
    const auto& cfg = config::Get();
    void* page = f.GetPage(s_layout, kNumberPage);
    if (!page) return;

    float xy[2] = {0.0f, 0.0f};
    const bool gotPos = AnchorPos(f, page0elem, anchor, xy);

    f.TextBind(page, kNumberPane, 0);

    auto cx = (unsigned char*)f.TextCtx;
    if (gotPos) {
        *(int*)(cx + 0xD70) = (int)xy[0];
        *(int*)(cx + 0xD74) = (int)xy[1];
    }
    // COLOUR. The context's colour word is four BYTES in memory, [R][G][B][A].
    // The PC's own TextBind proves the order by clearing the alpha as
    //     mov byte ptr [ctx+0xD83], 0
    // which is byte 3 of the word at 0xD80. Read back as a little-endian u32
    // that is A<<24 | B<<16 | G<<8 | R, so writing PS3's literal 0x00FFFF into
    // the low bits lands R=0xFF G=0xFF B=0x00 - YELLOW, which is exactly what
    // the screen showed. Build the word from named bytes so the order cannot be
    // got wrong again.
    //
    // PS3 (0x6987EC) swaps in cyan ONLY while its sequencer state == 2, and ORs
    // the panel's fade alpha in whichever branch ran.
    // FONT BANK. This is the write that was missing, and the reason the digits
    // came out in the label's face. ctx+0xDAC indexes three per-bank glyph
    // tables inside the context itself - the builder does
    //     mov ecx,[ctx+0xDAC] / shl rcx,8 / add rcx,charCode
    //     mov ebx,[ctx+rcx*4+0x14]
    // at +0x674967, and 0x14 + 3*256*4 == 0xC14 (the next table) says there are
    // exactly three. PS3 sets bank 1 for the records and the panel labels use
    // bank 0, which is the entire difference. Measured blind from the captures:
    // the PS3 "0" counter is a perfect rectangle (23/23 rows at 12px, fill
    // 1.000) against our ellipse (0.831) - a different face, not a resize, and
    // no width or height value could ever have produced it.
    //
    // ctx+0xDF0 is the companion metric mode; every shipped PC caller writes the
    // two as a pair. NOTE the offset: PS3 0x3C8 maps here at +0xA28, not the
    // +0xA18 that holds for the lower fields - PC inserted four dwords around
    // 0xDC4..0xDD8. The naive +0xA18 answer, 0xDE0, is a colour slot.
    //
    // Both must be written AFTER TextBind: it calls Reset (sub_2656B0), which
    // zeroes 0xDF0 and 0xDAC at +0x265713 / +0x265719.
    // Use the ranking face only once its sheet is RESIDENT. The load is async,
    // so the first frames legitimately fall back to bank 0 - and if the sheet
    // never arrives, the digits stay readable in the label face instead of
    // silently disappearing, which is what setting the bank blind did.
    int bank = cfg.PanelNumberFont;
    if (cfg.PanelRankingFont && RankingBankReady(f.TextCtx)) bank = 1;
    if (s_ovFont >= 0) bank = s_ovFont;
    {
        static int said = -1;
        if (said != bank) {
            said = bank;
            hoe::Log("number: drawing in font bank %d%s", bank,
                     bank == 1 ? "  <== the ranking face is live" : "");
        }
    }
    const int metric = s_ovMetric >= 0 ? s_ovMetric : cfg.PanelNumberMetric;
    *(int*)(cx + off::C_TEXT_CTX_METRIC_MODE) = metric;
    *(int*)(cx + off::C_TEXT_CTX_FONT_BANK)   = bank;

    const unsigned char R = highlight ? 0x00 : 0xFF;
    const unsigned char G = 0xFF;
    const unsigned char B = 0xFF;
    *(unsigned*)(cx + 0xD80) = ((unsigned)s_panelAlpha << 24) |
                               ((unsigned)B << 16) | ((unsigned)G << 8) | R;
    // 0xD88 stays a FLAT 0xFF, which is the configuration the digits were last
    // seen rendering in. PS3 does put its fade alpha here (0x370), but the
    // colour word above already carries that same byte, and if 0xD88 is a second
    // multiplier the two compound - 0x2A * 0x2A / 255 = 7, i.e. invisible during
    // the fade. Faithfulness is not worth reintroducing an unexplained
    // disappearance; `nr font` can set it if we want to test the theory.
    *(unsigned*)(cx + 0xD88) = s_ovAlpha >= 0 ? (unsigned)s_ovAlpha : 0xFFu;
    *(float*)(cx + off::C_TEXT_CTX_PRIO_FIELD) = (float)cfg.PanelTextPrio;

    // GLYPH WIDTH. sub_671020 is the size setter: it fans one value into the
    // height (0xD8C), half-height (0xD90) and line pitch (0xD9C), then derives
    // the width (0xDA0). The records do NOT go through it - PS3 stores the width
    // directly as `stw r5, 0x388(r4)` with r5 = 0x14, leaving the height at the
    // inherited 34. That is the whole of the "wrong font": the same face, but 34
    // wide instead of 20, so the digits came out round and squat where the PS3's
    // are tall and narrow. Width only - touching the height would rescale them.
    // THE ADVANCE IS HALVED BY THE BUILDER, so write twice what PS3 writes.
    //     +0x673CE7  mov eax, [rdi+0xDA0]
    //     +0x673CFC  shr eax, 1          <-- here
    // PS3 skips that halving whenever the metric mode is 4; the PC port kept the
    // metric-4 branch only in the SIZE SETTERS and dropped it throughout the
    // glyph pipeline, so on PC the shift is unconditional. Writing PS3's literal
    // 20 therefore produced an effective advance of 10 against 14px-wide ink -
    // a 4px overlap per digit, which is the cramped spacing. 40 halves to 20 and
    // matches PS3 exactly. Gated on bank 1, the bank that renders as class 2.
    {
        int adv = s_ovWidth >= 0 ? s_ovWidth : cfg.PanelNumberWidth;  // PS3's 20
        if (bank == 1) adv *= 2;
        *(int*)(cx + off::C_TEXT_CTX_GLYPH_W) = adv;
    }

    // GLYPH CELL WIDTH, and the reason the ranking face first drew as slivers.
    // A class-2 (fixed-grid ASCII) glyph takes its cell WIDTH from ctx+0xD90,
    // not from the height:
    //     +0x674892  cmp r14d, 2
    //     +0x674898  mov ecx, [rdi+0xD90]     <- class 2
    //     +0x6748A0  mov ecx, [rdi+0xD8C]     <- everything else
    // and TextBind's size setter always leaves 0xD90 = height>>1. The ranking
    // sheet's cells are SQUARE (512x256, a 16-column grid of 32x32), so a 32px
    // cell was being drawn into a 16px quad: every digit squeezed exactly 2:1.
    // Measured 11px of ink against the PS3's 22.5 before this, which is the
    // 2:1 the branch predicts. Only applied to the ranking bank - bank 0 is
    // proportional and takes the other branch, so it must keep its half-width.
    if (bank == 1) {
        const int cellW = s_ovCellW >= 0 ? s_ovCellW
                        : (cfg.PanelNumberCellW > 0
                               ? cfg.PanelNumberCellW
                               : *(int*)(cx + off::C_TEXT_CTX_GLYPH_H));
        *(int*)(cx + off::C_TEXT_CTX_CELL_W) = cellW;
    }

    if (config::Get().DiagTextBind) {
        static int said[2] = {0, 0};
        const int k = (anchor == kAnchorLatest) ? 0 : 1;
        if (said[k] < 2) {
            ++said[k];
            hoe::Log("  number %s: anchor 0x%X pos=(%.0f,%.0f)%s bank=%d mode=%d "
                     "glyph=%dx%d pitch=0x%X align=0x%X value=%d",
                     k ? "best" : "latest", anchor, xy[0], xy[1],
                     gotPos ? "" : " (POSITION UNAVAILABLE)",
                     *(int*)(cx + off::C_TEXT_CTX_FONT_BANK),
                     *(int*)(cx + off::C_TEXT_CTX_METRIC_MODE),
                     *(int*)(cx + off::C_TEXT_CTX_GLYPH_H),
                     *(int*)(cx + off::C_TEXT_CTX_GLYPH_W),
                     *(unsigned*)(cx + 0xD9C), *(unsigned*)(cx + 0xD78), value);
        }
    }

    // Time boards print PS3's composite value, not a bare integer. The format
    // string is byte-for-byte the one at EBOOT 0xEEA774, and the field
    // decomposition is its exact arithmetic - note the last field is V%100 and
    // not V/10%100, because the sub-second part stored in V is hundredths.
    if (s_timeMode) {
        const int v = value;
        if (v == kNoTimeRecord) {
            f.TextPrintf(f.TextCtx, "%s", kNoTimeText);   // never played
        } else {
            f.TextPrintf(f.TextCtx, "%01d:%02d'%02d\"%02d",
                         v / 3600000, (v / 60000) % 60, (v / 1000) % 60, v % 100);
        }
    } else {
        f.TextPrintf(f.TextCtx, "%d", value);
    }
}

}  // namespace

// Draw both records. Must run inside the render pass, before the flush.
void DrawNumbers() {
    if (!s_layout || !s_built || s_failed) return;
    if (!config::Get().PanelNumbers) return;
    Fns f;
    if (!Bind(f)) return;
    if (!f.TextBind || !f.TextPrintf || !f.TextCtx) return;
    const auto& cfg = config::Get();
    void* p0 = ElementOf(f, 0);
    if (!p0) return;

    // Ask for the face on the first frame the numbers draw. Doing it here rather
    // than at Open() means it runs on the game thread with a context that is
    // certainly live, and costs one branch afterwards.
    if (config::Get().PanelRankingFont) LoadRankingFont(f);

    // Advance the animation exactly once per drawn frame - see the note on
    // s_animFrame. Everything below is a pure function of this counter.
    const int frame = s_animFrame++;
    const int roll  = cfg.PanelRollFrames > 0 ? cfg.PanelRollFrames : 30;

    // THE COUNT-UP. PS3 holds a double at screen+0x1E8 and adds final/30.0 to it
    // every frame while its sequencer sits in state 2 (0x698D70..0x698D78), so
    // the Latest value climbs from zero to the achieved figure rather than
    // appearing outright. Truncation, not rounding: the original does fctidz.
    int shownLatest = s_latest;
    bool pulse = false;
    if (frame < roll) {
        shownLatest = 0;                       // the panel opens showing zero
    } else if (frame < roll * 2) {
        // THE SOUNDS, from the PS3 ranking code's own state machine: the roll-up
        // loop starts with the number and is kept by handle so it can be cut the
        // moment the number lands; the settle plays over the cut. See game.cpp.
        if (frame == roll && cfg.PanelSounds && s_rollSeHandle < 0 && s_latest > 0) {
            s_rollSeHandle = game::PlaySE((unsigned)cfg.PanelSeRoll);
        }
        s_rollAcc += (double)s_latest / (double)roll;
        shownLatest = (int)s_rollAcc;          // truncate
        pulse = cfg.PanelPulseLatest != 0;
    } else {
        if (frame == roll * 2 && cfg.PanelSounds && s_latest > 0) {
            if (s_rollSeHandle >= 0) { game::StopSE(s_rollSeHandle); s_rollSeHandle = -1; }
            game::PlaySE((unsigned)cfg.PanelSeSettle);
        }
        s_rollAcc = (double)s_latest;          // snap, so rounding cannot drift
        shownLatest = s_latest;
    }

    // THE BANNER. Page 9 of the layout carries the NEW RECORD art already; it
    // appears once the roll has settled and is taken away again a few seconds
    // later. Only on an actual new best - Show() is told by the recap step.
    if (cfg.PanelRecordBanner && s_newRecord) {
        // Full banner once the roll has settled, then park it as the badge and
        // leave it. PS3 never takes it away - the capture still shows the badge
        // top-right when the panel is dismissed.
        //
        // `>=` and a latch, not `==`: s_newRecord may only become true after
        // frame roll*3 has passed (the chase board's verdict comes from the
        // engine, seconds after the panel opens). Same timing as before when it
        // is known up front; raised at once when it is learned late.
        if (s_bannerAt < 0 && frame >= roll * 3) {
            s_bannerAt = frame;
            SetRecordBanner(Banner::Full);
            // PS3 fires this between the page-9 variant set and the 120-frame
            // hold - i.e. on exactly this frame.
            if (cfg.PanelSounds) game::PlaySE((unsigned)cfg.PanelSeNewRecord);
        }
        if (s_bannerAt >= 0 && frame == s_bannerAt + cfg.PanelBannerFrames) {
            SetRecordBanner(Banner::Parked);
        }
    }

    // ONE flash, not a loop. Classifying every frame of the PS3 capture for the
    // whole time the panel is up yields a single cyan run:
    //     25.37 -> 25.93  white   (0.56s)
    //     25.93 -> 26.43  CYAN    (0.50s)   <-- the only one
    //     26.43 -> 27.53  white   (1.10s, until the panel goes)
    // so the sequencer enters state 2 once and leaves. We have no sequencer, so
    // time it from the first frame the numbers draw. Best never pulses.


    DrawOneNumber(f, p0, kAnchorLatest, shownLatest, pulse, 1);
    // NO RECORD YET is not "a time of zero". GetBest returns 0 when a board has
    // never been played, and on a score board 0 reads correctly - but on a TIME
    // board it would format as 0:00'00"00, i.e. a flawless run nobody can beat.
    // PS3 shows its maximum instead, 99:59'59"99, which is also exactly the
    // value that makes the first real run win the comparison.
    const int shownBest = (s_timeMode && s_best <= 0) ? kNoTimeRecord : s_best;
    DrawOneNumber(f, p0, kAnchorBest,   shownBest, false, 2);
}


// Override the glyph fields live. -1 on any argument restores the INI value.
//
// The point of this is search cost: the font bank is one of exactly three
// values, the metric mode one of a handful, and the only way to tell which is
// right is to look at the screen. Without this each probe costs a game restart
// and a walk back to the panel.
void SetNumberFont(int bank, int metric, int width, int alpha, int cellW) {
    s_ovFont   = bank;
    s_ovMetric = metric;
    s_ovWidth  = width;
    s_ovAlpha  = alpha;
    s_ovCellW  = cellW;
    // Re-arm the per-slot diagnostics so the next frame logs what these produce.
    hoe::Log("nr font: bank=%d metric=%d width=%d alpha=%d cellW=%d (-1 = use the INI)",
             bank, metric, width, alpha, cellW);
}

// Dump the per-bank font state, to answer "is this bank usable" by reading.
//
// Corrected model. ctx+0xC14 is NOT 37 metrics per bank - it is one FLAT 86-entry
// kigou table spanning 0xC14..0xD6C (86 iterations in the ctor at +0x66E280,
// bounded by the ctx+0xD6C write at +0x66E297), which the resolver strides by
// bank*36. Reading it as bank*37 was wrong and reported a meaningless number.
//
// What actually decides whether a bank can draw ASCII is the HANKAKU sheet
// handle at ctx+0x08[bank] - the resolver's `else` branch at +0x66EB3C - fed
// from the global 0x1990EA8[bank] by the loader sub_670520. ctx+0xD6C is the
// font STYLE that picks which face a nonzero bank loads: 0 none, 2 caba,
// 3 darts. The PS3 had 1 mobile and 4 ranking as well; both of those prefix
// strings were deleted from the PC exe.
void LogFontTables() {
    Fns f;
    if (!Bind(f) || !f.TextCtx) { hoe::Log("fonttab: no text context"); return; }
    auto cx = (const unsigned char*)f.TextCtx;
    const uintptr_t res = hoe::Base() + 0x1990EC0;   // per-char resource pointers
    hoe::Log("fonttab: context %p  style(ctx+0xD6C)=%d  (0 none, 2 caba, 3 darts)",
             f.TextCtx, *(const int*)(cx + 0xD6C));
    for (int bank = 0; bank < 3; ++bank) {
        const int hankaku = *(const int*)(cx + 0x08 + bank * 4);
        int live = 0;
        auto slot = (const void* const*)(res + (uintptr_t)bank * 0x800);
        for (int i = 0; i < 256; ++i) if (slot[i]) ++live;
        hoe::Log("  bank %d: hankaku sheet=%d  zenkaku resources %d/256%s",
                 bank, hankaku, live,
                 (hankaku == -1 && live == 0)
                     ? "   <== NO FONT LOADED in this bank" : "");
    }
}


// Override the score/time kind from the engine's own field rather than from the
// board-name flag scan. CActionColosseumExtra+0x1B8 is what the game itself
// branches on at +0x42E584 to pick the clock layout over the kill counter, so it
// is authoritative; PickBoardName's flag scan stays as a cross-check.
void SetTimeMode(bool on) {
    if (s_timeMode != on) hoe::Log("netrank: time mode %s", on ? "ON" : "off");
    s_timeMode = on;
}


// Raise or lower page 9, which carries the NEW RECORD art.
//
// Nothing is drawn here - the banner is baked into the layout this mod already
// loads (2d_cf_net_tranking02.dds, atlas index 1). Three steps, mirroring what
// the engine's own page setup does: clear the pause bit the builder set, drop
// the suppress flag, and restart the animation clock.
//
// The third call is sub_48A520, which this project's notes long listed as
// "never call - rebuilds the pane tree". That was true only of a variant
// CHANGE: +0x48A562 compares the requested variant with the current one and
// jumps past the rebuild when they match. Page 9 is already variant 0, so
// asking for variant 0 skips the rebuild and only resets the clock (+0x48A594)
// and the speed (+0x48A59D). Flags bit 4 makes the variant index literal.
// This is also why the banner is HIDDEN again rather than parked on variant 1:
// switching variants is the one thing here that would trigger the rebuild.
// Banner states. PS3 does not hide the banner - after the full-screen version
// has played it switches to variant 1, a small "NEW RECORD" badge parked at the
// top right of the panel, and leaves it there. Measured from the capture: the
// big banner runs about 1.3s and then the badge persists to the end of the
// recap, with the rows readable again behind it.
void SetRecordBanner(Banner state) {
    if (!s_layout) return;
    Fns f;
    if (!Bind(f)) return;
    const int page = (int)off::C_RECORD_BANNER_PAGE;
    if (page < 0 || page >= s_pages) {
        hoe::Log("banner: page %d out of range (0..%d)", page, s_pages - 1);
        return;
    }
    void* e = ElementOf(f, page);
    if (!e) { hoe::Log("banner: page %d has no element", page); return; }

    const auto& r = resolve::Get();
    auto setVariant = r.SetAnimVariant
        ? (void(*)(void*, unsigned, unsigned))(hoe::Base() + r.SetAnimVariant)
        : nullptr;

    if (state == Banner::Hidden) {
        f.SetSuppress(e, 1);
    } else {
        *(unsigned*)((unsigned char*)e + off::C_ELEM_FLAGS_FIELD) &= ~0x02u;
        f.SetSuppress(e, 0);
        // Variant 0 is the full banner, variant 1 the parked badge. Asking for
        // the variant the page already has skips the pane-tree rebuild
        // (+0x48A562) and only restarts the clock; asking for a DIFFERENT one
        // does rebuild - which is what the park costs, and is exactly what the
        // engine's own page constructor does at +0x4842AB, so it is a normal
        // operation rather than something exotic.
        if (setVariant) setVariant(e, state == Banner::Full ? 0u : 1u, 4u);
    }
    s_bannerUp = (state != Banner::Hidden);
    hoe::Log("banner: NEW RECORD %s (page %d)",
             state == Banner::Hidden ? "hidden"
                                     : (state == Banner::Full ? "SHOWN (full)"
                                                              : "parked as a badge"),
             page);
}

// Two-state shim so nothing else has to know about the parked state.
void ShowRecordBanner(bool on) {
    SetRecordBanner(on ? Banner::Full : Banner::Hidden);
}

// Told by the recap step whether this run beat the stored best.
void SetNewRecord(bool on) {
    s_newRecord = on;
    hoe::Log("netrank: new record = %s", on ? "YES" : "no");
}

}  // namespace netrank
