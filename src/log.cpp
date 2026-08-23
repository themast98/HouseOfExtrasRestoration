#include "log.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

namespace hoe {

static const char* kPath = "HouseOfExtras.log";

void LogInit() {
    FILE* f = fopen(kPath, "w");
    if (f) { fprintf(f, "=== House of Extras ASI ===\n"); fclose(f); }
}

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    FILE* f = fopen(kPath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fclose(f);
}

}  // namespace hoe
