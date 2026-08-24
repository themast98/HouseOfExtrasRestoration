#include "config.h"
#include "log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace config {
namespace {

// Absolute path next to the game exe; GetPrivateProfile* resolves bare names
// against the Windows directory, which is never what we want.
const char* IniPath() {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) { path[0] = 0; return path; }
        char* slash = strrchr(path, '\\');
        if (!slash) { path[0] = 0; return path; }
        const size_t used = (size_t)(slash + 1 - path);
        if (snprintf(slash + 1, sizeof(path) - used, "%s", "HouseOfExtras.ini")
                >= (int)(sizeof(path) - used)) {
            path[0] = 0;   // truncated - fall back to built-in defaults
        }
    }
    return path;
}

// Written on first run so users get a documented file to edit, the way
// Like A Brawler 8's IniSettings.Write() does.
//
// NOTE: every key here MUST also be read in Load(). A key that is documented
// but never read silently falls back to the C++ default, which is how an
// unlimited result-screen wait once slipped through.
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
        "\n;  Screen id opened for the results screen. Valid range 0-310.\n"
        ";  225 = pjs_dlc_result (default) - the real recap. Loads tougijyo_all.bin,\n"
        ";        allocates a pad listener, and closes itself on the confirm button.\n"
        ";  220 = pjs_dlc_survivalbtl_end - DO NOT USE. It constructs and registers,\n"
        ";        but its mode field is left at 4 (outside its own 0..3 switch), so its\n"
        ";        draw and update both early-out: it never renders and never closes.\n"
        "ResultScreenId=225\n"
        "\n;  Seconds to wait for you to dismiss the results screen (0-3600).\n"
        ";  120 is far longer than anyone needs to read a recap, and guarantees the\n"
        ";  game can never sit forever on a screen that will not close.\n"
        ";  0 = wait indefinitely (only if you are sure the screen is dismissible).\n"
        "ResultTimeoutSec=120\n"
        "\n;  1 = track best score per mode in HouseOfExtras.records\n"
        ";  Currently 0: the in-memory score field has NOT been identified yet, so\n"
        ";  enabling this would persist a meaningless number.\n"
        "TrackBestScore=0\n"
        "\n;  1 = dump candidate object fields to the log at results time.\n"
        ";  Only useful while hunting the score offset against a known score.\n"
        "DiagDumpFields=0\n"
        "\n;  1 = log the result screen's own state machine once a second, so we can\n"
        ";  tell whether the engine is actually updating it or it is inert.\n"
        "DiagScreenState=1\n"
        "\n;  1 = call the transition/fade helper the reference handler calls before\n"
        ";  opening the screen. Set to 0 to test whether that fade is what leaves the\n"
        ";  display black with the screen up.\n"
        "CallTransitionSetup=1\n", f);
    fclose(f);
}

Settings Load() {
    Settings s;
    const char* p = IniPath();
    if (!p[0]) { hoe::Log("config: could not build ini path, using built-in defaults"); return s; }

    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaults(p);
        hoe::Log("wrote default HouseOfExtras.ini");
    }
    s.Enabled          = GetPrivateProfileIntA("General", "Enabled",          s.Enabled,          p);
    s.ShowResultScreen = GetPrivateProfileIntA("Results", "ShowResultScreen", s.ShowResultScreen, p);
    s.ResultScreenId   = GetPrivateProfileIntA("Results", "ResultScreenId",   s.ResultScreenId,   p);
    s.ResultTimeoutSec = GetPrivateProfileIntA("Results", "ResultTimeoutSec", s.ResultTimeoutSec, p);
    s.TrackBestScore   = GetPrivateProfileIntA("Results", "TrackBestScore",   s.TrackBestScore,   p);
    s.DiagDumpFields   = GetPrivateProfileIntA("Results", "DiagDumpFields",   s.DiagDumpFields,   p);
    s.DiagScreenState  = GetPrivateProfileIntA("Results", "DiagScreenState",  s.DiagScreenState,  p);
    s.CallTransitionSetup =
        GetPrivateProfileIntA("Results", "CallTransitionSetup", s.CallTransitionSetup, p);

    // An unvalidated id indexes the screen-slot array (mainMgr + 0x1E8 + id*8).
    // slot(311) lands exactly on mainMgr+0xBA0, so 310 is the hard ceiling.
    if (s.ResultScreenId < 0 || s.ResultScreenId > 310) {
        hoe::Log("config: ResultScreenId=%d out of range 0-310, using 225", s.ResultScreenId);
        s.ResultScreenId = 225;
    }
    // Guards against ResultTimeoutSec * 60 overflowing.
    if (s.ResultTimeoutSec < 0 || s.ResultTimeoutSec > 3600) {
        hoe::Log("config: ResultTimeoutSec=%d out of range 0-3600, using 120", s.ResultTimeoutSec);
        s.ResultTimeoutSec = 120;
    }

    hoe::Log("config: Enabled=%d ShowResultScreen=%d ResultScreenId=%d ResultTimeoutSec=%d "
             "TrackBestScore=%d DiagDumpFields=%d",
             s.Enabled, s.ShowResultScreen, s.ResultScreenId, s.ResultTimeoutSec,
             s.TrackBestScore, s.DiagDumpFields);
    return s;
}

}  // namespace

const Settings& Get() { static Settings s = Load(); return s; }

}  // namespace config
