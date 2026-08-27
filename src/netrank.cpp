#include "netrank.h"
#include "game.h"
#include "resolve.h"
#include "offsets.h"
#include "patch.h"
#include "log.h"

#include <string.h>

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
};

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
    return true;
}

// page -> element, both checked. Returns null if either is missing.
void* ElementOf(const Fns& f, int i) {
    void* page = f.GetPage(s_layout, i);      // returns null if pages is null;
    if (!page) return nullptr;                // never bounds-checks i, hence s_pages
    return *(void**)((unsigned char*)page + off::C_PAGE_ELEM_FIELD);
}

}  // namespace

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
    s_frames = 0;
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

void Show(int latest, int best) {
    if (!s_layout || !s_built || s_shown) return;
    Fns f;
    if (!Bind(f)) return;

    // The builder creates every page suppressed. Clearing that on page 0 is the
    // whole of "show it" - the render pass picks the element up off the global
    // list by itself on the next frame.
    void* elem = ElementOf(f, 0);
    if (!elem) { hoe::Log("netrank: page 0 element vanished"); return; }
    f.SetSuppress(elem, 0);

    s_shown = true;
    hoe::Log("netrank: page 0 unsuppressed (latest=%d best=%d) - the numbers are "
             "not wired up yet, this is the layout only", latest, best);
}

void Draw() {
    // Deliberately empty. Drawing is done by the engine's own render pass off
    // the global element list; there is nothing to call per frame.
}

void Close() {
    if (!s_layout) return;
    Fns f;
    if (!Bind(f)) { s_layout = nullptr; return; }

    // ReleaseLayout is not optional and not merely a free: it destroys the page
    // array through its vector-deleting destructor, and that is what UNLINKS
    // each element from the global render list. Skip it and the renderer keeps
    // walking freed memory a frame later.
    f.ReleaseLayout(s_layout);
    f.Free(s_layout);
    hoe::Log("netrank: released layout %p (slot %d)", s_layout, s_slot);

    s_layout = nullptr;
    s_slot = -1;
    s_pages = 0;
    s_built = s_shown = s_failed = false;
    s_frames = 0;
}

}  // namespace netrank
