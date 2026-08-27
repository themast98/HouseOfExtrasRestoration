#include "netrank.h"
#include "game.h"
#include "resolve.h"
#include "offsets.h"
#include "patch.h"
#include "log.h"

namespace netrank {
namespace {

// The panel object the sibling widget uses: 0x28 bytes, layout pointer at +0,
// two bound panes at +8 and +0x10, and the int it prints as %d at +0x20.
constexpr int  kPanelSize  = 0x28;
constexpr int  kLayoutSize = 0x128;
constexpr int  kValueField = 0x20;
constexpr int  kHeapUi     = 10;

// The layout the DELETED code drew. The surviving widget's own loader
// hardcodes "pjs_mg_net_ranking" (the minigame board), which is why we call
// LoadLayout ourselves instead of reusing NETRANK_LOADLAYOUT.
const char kLayoutName[] = "pjs_net_ranking";

void*  s_panel  = nullptr;
bool   s_bound  = false;
bool   s_shown  = false;
int    s_frames = 0;

}  // namespace

bool IsOpen() { return s_panel != nullptr; }

bool Open() {
    const auto& r = resolve::Get();
    if (!r.netRankOk) { hoe::Log("netrank: pieces did not resolve, cannot build"); return false; }
    if (s_panel)      { hoe::Log("netrank: already open"); return true; }

    const uintptr_t b = hoe::Base();
    auto pushHeap = (void(*)(int, int))          (b + r.HeapPush);
    auto alloc    = (void*(*)(unsigned long long))(b + r.HeapAlloc);
    auto popHeap  = (void(*)())                   (b + r.HeapPop);
    auto ctor     = (void*(*)(void*))             (b + r.NetRankCtor);
    auto loadLay  = (void*(*)(void*, const char*, int))(b + r.LayoutLoad);

    // The engine's own screen factories scope their allocations to the UI heap
    // before constructing, so do the same rather than allocating wherever the
    // caller happens to have left the current heap.
    pushHeap(kHeapUi, 0);
    void* panel = alloc(kPanelSize);
    if (panel) {
        ctor(panel);                                  // zeroes all 0x28 bytes
        // Mirror NETRANK_LOADLAYOUT exactly: allocate the layout, load into it,
        // and store the LOADER'S RETURN (not the allocation) at panel[0]. If the
        // allocation fails it stores null, which the widget then treats as
        // "no layout" rather than faulting.
        void* mem = alloc(kLayoutSize);
        void* lay = mem ? loadLay(mem, kLayoutName, 0) : nullptr;
        *(void**)panel = lay;
        hoe::Log("netrank: panel=%p layout=%p ('%s')", panel, lay, kLayoutName);
    }
    popHeap();

    if (!panel || !*(void**)panel) {
        hoe::Log("netrank: allocation or layout load failed");
        // The panel block itself is engine-owned once allocated; leave it be
        // rather than guessing at a free that may not match the heap it came from.
        s_panel = nullptr;
        return false;
    }
    s_panel = panel;
    s_bound = s_shown = false;
    s_frames = 0;
    return true;
}

bool Ready() {
    if (!s_panel) return false;
    if (s_bound)  return true;

    const auto& r = resolve::Get();
    const uintptr_t b = hoe::Base();
    auto bind = (void(*)(void*))(b + r.NetRankBind);

    ++s_frames;
    // Bind builds the pages, but only once the csb has actually arrived - it
    // bails while the load is still in flight - so it is called every frame
    // until the layout reports that it has pages.
    bind(s_panel);
    void* layout = *(void**)s_panel;
    const int hasPages = (layout && game::LayoutHasPages) ? game::LayoutHasPages(layout) : 0;
    if (hasPages) {
        s_bound = true;
        hoe::Log("netrank: bound after %d frames; panes=%p,%p", s_frames,
                 *(void**)((unsigned char*)s_panel + 8),
                 *(void**)((unsigned char*)s_panel + 0x10));
        game::LogLayoutResources("netrank", layout);
    } else if (s_frames % 60 == 0) {
        hoe::Log("netrank: still loading after %d frames", s_frames);
    }
    return s_bound;
}

void Show(int latest, int best) {
    if (!s_panel || !s_bound || s_shown) return;
    const auto& r = resolve::Get();
    const uintptr_t b = hoe::Base();
    auto show = (void(*)(void*, int, int))(b + r.NetRankShow);

    // One int field exists on this widget; the second number needs a pane the
    // minigame version does not use, so `best` is logged for now and wired up
    // once the live panel shows which panes pjs_net_ranking actually has.
    *(int*)((unsigned char*)s_panel + kValueField) = latest;

    // Show() indexes panel[8 + group*8] and dereferences it. Bind fills those
    // from the layout's panes, and pjs_net_ranking need not have the same panes
    // as the minigame board this widget was written for - so a group whose pane
    // did not bind is skipped rather than passed to the engine as null.
    auto pane = [&](int g) { return *(void**)((unsigned char*)s_panel + 8 + g * 8); };
    for (int g = 0; g < 2; ++g) {
        if (pane(g)) show(s_panel, g, 1);
        else hoe::Log("netrank: group %d has no bound pane, not showing it", g);
    }
    s_shown = true;
    hoe::Log("netrank: shown  latest=%d best=%d  (panel %p, value field +0x%X)",
             latest, best, s_panel, kValueField);
}

void Draw() {
    if (!s_panel || !s_shown) return;
    const auto& r = resolve::Get();
    auto draw = (void(*)(void*))(hoe::Base() + r.NetRankDraw);
    draw(s_panel);
}

void Close() {
    if (!s_panel) return;
    const auto& r = resolve::Get();
    const uintptr_t b = hoe::Base();
    auto show = (void(*)(void*, int, int))(b + r.NetRankShow);
    auto pane = [&](int g) { return *(void**)((unsigned char*)s_panel + 8 + g * 8); };
    // Hide before dropping the reference. The widget's own release path was
    // never resolved to a unique signature, so the panel and its layout are
    // deliberately LEAKED rather than freed with a guessed deallocator - a
    // handful of bytes once per run, against the risk of a bad free.
    for (int g = 0; g < 2; ++g) if (pane(g)) show(s_panel, g, 0);
    hoe::Log("netrank: closed (panel %p intentionally not freed)", s_panel);
    s_panel = nullptr;
    s_bound = s_shown = false;
    s_frames = 0;
}

}  // namespace netrank
