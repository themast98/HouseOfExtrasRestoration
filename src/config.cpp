#include "config.h"
#include "log.h"
#include <windows.h>
#include <stdio.h>

namespace config {
namespace {

// Absolute path next to the game exe; GetPrivateProfile* resolves bare names
// against the Windows directory, which is never what we want.
const char* IniPath() {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "HouseOfExtras.ini");
    }
    return path;
}

// Written on first run so users get a documented file to edit, the way
// Like A Brawler 8's IniSettings.Write() does.
void WriteDefaults(const char* p) {
    FILE* f = fopen(p, "w");
    if (!f) return;
    fputs(
        "; House of Extras Restoration - runtime settings\n"
        "; All values are integers. Delete this file to regenerate defaults.\n"
        "\n[General]\n"
        ";  0 = do nothing (mod disabled)\n"
        ";  1 = install the fix\n"
        "Enabled=1\n"
        "\n[Results]\n"
        ";  0 = skip the results screen, return straight to Naomi's Palace\n"
        ";  1 = show the results screen\n"
        "ShowResultScreen=1\n"
        "\n;  Screen id opened for the results screen.\n"
        ";  220 = pjs_dlc_survivalbtl_end (default, the Extras results layout)\n"
        ";  225 = pjs_dlc_result (the Coliseum tournament result layout)\n"
        "ResultScreenId=220\n"
        "\n;  0 = do not track best scores\n"
        ";  1 = track best score per mode in HouseOfExtras.records\n"
        "TrackBestScore=1\n", f);
    fclose(f);
}

Settings Load() {
    Settings s;
    const char* p = IniPath();
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaults(p);
        hoe::Log("wrote default HouseOfExtras.ini");
    }
    s.Enabled          = GetPrivateProfileIntA("General", "Enabled",          s.Enabled,          p);
    s.ShowResultScreen = GetPrivateProfileIntA("Results", "ShowResultScreen", s.ShowResultScreen, p);
    s.ResultScreenId   = GetPrivateProfileIntA("Results", "ResultScreenId",   s.ResultScreenId,   p);
    s.TrackBestScore   = GetPrivateProfileIntA("Results", "TrackBestScore",   s.TrackBestScore,   p);
    s.ResultTimeoutSec = GetPrivateProfileIntA("Results", "ResultTimeoutSec", s.ResultTimeoutSec, p);
    hoe::Log("config: Enabled=%d ShowResultScreen=%d ResultScreenId=%d TrackBestScore=%d ResultTimeoutSec=%d",
             s.Enabled, s.ShowResultScreen, s.ResultScreenId, s.TrackBestScore, s.ResultTimeoutSec);
    return s;
}

}  // namespace

const Settings& Get() { static Settings s = Load(); return s; }

}  // namespace config
