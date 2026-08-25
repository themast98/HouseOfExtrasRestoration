#include "paths.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace hoe {

const char* ExeRelative(const char* leaf) {
    // One cache slot per distinct leaf pointer. There are only three callers
    // (ini, records, log), each passing a string literal, so a tiny table beats
    // dragging in std::map for this.
    struct Entry { const char* leaf; char path[MAX_PATH]; bool ok; };
    static Entry cache[4] = {};
    static int used = 0;

    for (int i = 0; i < used; ++i) {
        if (cache[i].leaf == leaf) return cache[i].ok ? cache[i].path : nullptr;
    }
    if (used >= (int)(sizeof(cache) / sizeof(cache[0]))) return nullptr;

    Entry& e = cache[used++];
    e.leaf = leaf;
    e.ok = false;

    const DWORD n = GetModuleFileNameA(nullptr, e.path, MAX_PATH);
    // n == 0 is failure; n == MAX_PATH means it was truncated, and a truncated
    // path would put our file in the wrong place, so refuse both.
    if (n == 0 || n >= MAX_PATH) return nullptr;

    char* slash = strrchr(e.path, '\\');
    if (!slash) return nullptr;

    const size_t used_bytes = (size_t)(slash + 1 - e.path);
    // snprintf, not strcpy: a deep install directory would otherwise overrun
    // the buffer by however much the leaf name does not fit.
    if (snprintf(slash + 1, sizeof(e.path) - used_bytes, "%s", leaf)
            >= (int)(sizeof(e.path) - used_bytes)) {
        return nullptr;
    }
    e.ok = true;
    return e.path;
}

}  // namespace hoe
