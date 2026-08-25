#include "log.h"
#include "paths.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

namespace hoe {

static const char* kLeaf = "HouseOfExtras.log";

// Next to the game exe, so the log is always where the ini and records are.
// It used to be a bare relative name, which lands wherever the working
// directory happens to point - fine under Steam, confusing anywhere else.
static const char* LogPath() {
    const char* p = ExeRelative(kLeaf);
    return p ? p : kLeaf;
}

void LogInit() {
    FILE* f = fopen(LogPath(), "w");
    if (f) { fprintf(f, "=== House of Extras ASI ===\n"); fclose(f); }
}

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    FILE* f = fopen(LogPath(), "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fclose(f);
}

}  // namespace hoe
