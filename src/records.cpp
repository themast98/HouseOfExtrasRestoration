#include "records.h"
#include "log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace records {
namespace {

struct Entry { int mode; int score; };
Entry g[32];
int   gCount = 0;
bool  gLoaded = false;

const char* Path() {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "HouseOfExtras.records");
    }
    return path;
}

void Load() {
    if (gLoaded) return;
    gLoaded = true;
    FILE* f = fopen(Path(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f) && gCount < 32) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int k = 0, v = 0;
        if (sscanf(line, "%d=%d", &k, &v) == 2) { g[gCount].mode = k; g[gCount].score = v; ++gCount; }
    }
    fclose(f);
}

void Save() {
    FILE* f = fopen(Path(), "w");
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
    if (score <= GetBest(modeId)) return false;
    for (int i = 0; i < gCount; ++i) {
        if (g[i].mode == modeId) { g[i].score = score; Save(); return true; }
    }
    if (gCount < 32) { g[gCount].mode = modeId; g[gCount].score = score; ++gCount; Save(); return true; }
    return false;
}

}  // namespace records
