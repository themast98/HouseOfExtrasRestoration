#include "records.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace records {
namespace {

struct Entry { int mode; int score; };
Entry g[32];
int   gCount = 0;
bool  gLoaded = false;

// No House of Extras run can plausibly exceed this. Anything above it did not
// come from real play - an early build of this mod wrote 1932486006 after
// reading the wrong object field. Such a value would otherwise sit in the file
// forever as an unbeatable "best" and silently mask every correct score, so it
// is rejected on BOTH load and store and the file heals itself on next write.
constexpr int kMaxPlausibleScore = 9999;

bool Plausible(int score) { return score > 0 && score <= kMaxPlausibleScore; }

// Was a hand-rolled GetModuleFileName + unbounded strcpy, which overruns the
// buffer when the install directory is deep enough that the leaf name no
// longer fits. config.cpp guarded the same operation; this copy did not.
const char* Path() { return hoe::ExeRelative("HouseOfExtras.records"); }

void Load() {
    if (gLoaded) return;
    gLoaded = true;
    const char* path = Path();
    if (!path) { hoe::Log("records: could not build a path; best scores off"); return; }
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f) && gCount < 32) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int k = 0, v = 0;
        if (sscanf(line, "%d=%d", &k, &v) != 2) continue;
        if (!Plausible(v)) {
            hoe::Log("records: discarding implausible stored best %d=%d", k, v);
            continue;   // dropped here, and gone from the file on the next Save()
        }
        g[gCount].mode = k; g[gCount].score = v; ++gCount;
    }
    fclose(f);
}

void Save() {
    const char* path = Path();
    if (!path) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs("# House of Extras best scores (modeId=score)\n", f);
    for (int i = 0; i < gCount; ++i) fprintf(f, "%d=%d\n", g[i].mode, g[i].score);
    fclose(f);
}

}  // namespace

int GetBest(int modeId) {
    Load();
    for (int i = 0; i < gCount; ++i) if (g[i].mode == modeId) return g[i].score;
    return 0;
}

bool SetBest(int modeId, int score) {
    Load();
    if (!Plausible(score)) {
        hoe::Log("records: refusing to store implausible score %d for mode %d", score, modeId);
        return false;
    }
    if (score <= GetBest(modeId)) return false;
    for (int i = 0; i < gCount; ++i) {
        if (g[i].mode == modeId) { g[i].score = score; Save(); return true; }
    }
    if (gCount < 32) { g[gCount].mode = modeId; g[gCount].score = score; ++gCount; Save(); return true; }
    return false;
}

}  // namespace records
