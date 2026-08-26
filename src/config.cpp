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
        ";  0 = no result screen (default), return straight to Naomi's Palace\n"
        ";  1 = open ResultScreenId when the mode ends\n"
        ";\n"
        ";  WHY THE DEFAULT IS 0. The original House of Extras end screen was an\n"
        ";  ONLINE LEADERBOARD, not a local recap. The PS3 build still contains\n"
        ";  d:/Project/Soul_oe/src/ranking/ranking.cpp, with ranking_regist_thread\n"
        ";  and ranking_reference_thread, and board names like 'Battle King\n"
        ";  Ranking (Kiryu)', 'Fastest Killer Ranking (Saejima)' and 'Speed King\n"
        ";  Ranking' - one per mode, per character - drawn with pjs_net_ranking.\n"
        ";  Sega deleted that module for the PC release because the PS3 service it\n"
        ";  talked to no longer exists.\n"
        ";\n"
        ";  So there is no local results screen to point at. Every candidate tried\n"
        ";  (220 caption, 222 survival-battle, 225 tournament) belongs to another\n"
        ";  mode and renders nothing here. Setting this to 1 opens one anyway,\n"
        ";  which is useful only for experimenting.\n"
        "ShowResultScreen=0\n"
        "\n;  Screen id opened for the results screen. Valid range 0-310.\n"
        ";  The id picks a CLASS through the engine's factory table, and they are\n"
        ";  NOT interchangeable:\n"
        ";  Each result screen belongs to a MACRO FAMILY, and the untouched\n"
        ";  handlers show which goes with which:\n"
        ";      CMissionMacroColosseum        -> 225\n"
        ";      CMissionMacroSurvivalBattle   -> 222\n"
        ";      CMissionMacroAdventureSurvive -> 223\n"
        ";      CMissionMacroColosseumExtra   -> deleted; this mod supplies it\n"
        ";\n"
        ";  225 = CActionTougijyoAllStarResult (default) - what the untouched\n"
        ";        CMissionMacroColosseum opens for this exact step, and House of\n"
        ";        Extras runs on its direct sibling. Safe: every pointer its\n"
        ";        constructor touches is null-checked.\n"
        ";  222 = CActionSurvivalBattleResult - belongs to the survival-battle\n"
        ";        flow. Its constructor loads screen 217 and dereferences it\n"
        ";        WITHOUT a null check, so opening it from any other mode is a\n"
        ";        hard crash. The mod refuses unless 217 is already live.\n"
        ";  223 = CActionSurvivalOnigokkoResult - the tag-mode result.\n"
        ";  220 = CActionSurvivalCaption - a CAPTION BANNER, not a recap at all.\n"
        ";        This was the default for a long time and is exactly why nothing\n"
        ";        ever appeared: the object was healthy, it just was not the recap.\n"
        "ResultScreenId=225\n"
        "\n;  Caption banners only (ResultScreenId=220): which of the four banner\n"
        ";  pages to show, 0-3 - svbtl_clear, svbtl_congra, svbtl_mission,\n"
        ";  svbtl_boss / oni_timeover. Ignored by the real result actions, which\n"
        ";  choose their own content.\n"
        "ResultVariant=0\n"
        "\n;  Caption banners only: frames to hold before the close animation.\n"
        ";  Retail call sites use 30, 40 and 180. Result actions time themselves.\n"
        "ResultHoldFrames=180\n"
        "\n;  Seconds to wait for you to dismiss the results screen (0-3600).\n"
        ";  120 is far longer than anyone needs to read a recap, and guarantees the\n"
        ";  game can never sit forever on a screen that will not close.\n"
        ";  0 = wait indefinitely (only if you are sure the screen is dismissible).\n"
        "ResultTimeoutSec=120\n"
        "\n;  1 = track best score per mode in HouseOfExtras.records\n"
        ";  The score field is CActionColosseumExtra+0x208, confirmed against runs\n"
        ";  of 3, 4 and 6 defeated enemies. Scores outside 1..9999 are rejected on\n"
        ";  both read and write, so a bad value cannot become a permanent 'best'.\n"
        "TrackBestScore=1\n"
        "\n;  1 = dump candidate object fields to the log at results time.\n"
        ";  Only useful while hunting the score offset against a known score.\n"
        "DiagDumpFields=0\n"
        "\n;  1 = log the result screen's own state machine once a second, so we can\n"
        ";  tell whether the engine is actually updating it or it is inert.\n"
        "DiagScreenState=1\n"
        "\n;  1 = call the transition/fade helper the reference handler calls before\n"
        ";  opening the screen. Set to 0 to test whether that fade is what leaves the\n"
        ";  display black with the screen up.\n"
        "CallTransitionSetup=1\n"
        "\n;  1 = after opening the screen, fade the black overlay back OUT.\n"
        ";  Verified live: the battle-end flow leaves a full-screen OPAQUE black\n"
        ";  overlay over everything, and clearing it does reveal 2D UI hidden\n"
        ";  underneath - but NOT the results panel, which stays absent.\n"
        ";  (This used mode 0, which actually targets alpha 1.0 and so faded TO\n"
        ";  black; it now uses a mode that targets alpha 0.)\n"
        ";  Diagnostic aid - the black background is correct in the original game.\n"
        "ForceFadeIn=0\n"
        "\n[Modes]\n"
        ";  1 = show all of Bob's House of Extras rows, not just Battle King.\n"
        ";  The other rows are gated on save flags the game sets from DLC\n"
        ";  entitlements, and the PC build never grants entitlements 0x17-0x19,\n"
        ";  so they can never appear. This grants them and sets those flags via\n"
        ";  the game's own setter. It writes to your save.\n"
        ";  0 = leave the menu exactly as the PC build ships it.\n"
        "UnlockAllModes=1\n"
        "\n;  1 = log which mission macro is running and what step it is on.\n"
        ";  Cheap (sampled every 2 seconds, capped at 300 lines) and it works for\n"
        ";  every mode, including ones this mod does not patch. If any mode ever\n"
        ";  hangs, the log names the exact class and step it parked on instead of\n"
        ";  leaving you with a black screen and nothing to go on.\n"
        "DiagMissionWatch=1\n"
        "\n;  1 = hold the recap on screen indefinitely and poll HouseOfExtras.cmd\n"
        ";  for commands, replying in HouseOfExtras.reply.\n"
        ";  A development aid: it makes the recap inspectable and experimentable\n"
        ";  live, instead of needing a fresh play session for every attempt.\n"
        ";  Send `resume` to let it finish normally. Leave at 0 for play.\n"
        "RecapFreeze=0\n", f);
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
    s.ResultVariant    = GetPrivateProfileIntA("Results", "ResultVariant",    s.ResultVariant,    p);
    s.ResultHoldFrames = GetPrivateProfileIntA("Results", "ResultHoldFrames", s.ResultHoldFrames, p);
    s.ResultTimeoutSec = GetPrivateProfileIntA("Results", "ResultTimeoutSec", s.ResultTimeoutSec, p);
    s.TrackBestScore   = GetPrivateProfileIntA("Results", "TrackBestScore",   s.TrackBestScore,   p);
    s.DiagDumpFields   = GetPrivateProfileIntA("Results", "DiagDumpFields",   s.DiagDumpFields,   p);
    s.DiagScreenState  = GetPrivateProfileIntA("Results", "DiagScreenState",  s.DiagScreenState,  p);
    s.CallTransitionSetup =
        GetPrivateProfileIntA("Results", "CallTransitionSetup", s.CallTransitionSetup, p);
    s.ForceFadeIn      = GetPrivateProfileIntA("Results", "ForceFadeIn",      s.ForceFadeIn,      p);
    s.UnlockAllModes   = GetPrivateProfileIntA("Modes",   "UnlockAllModes",   s.UnlockAllModes,   p);
    s.DiagMissionWatch = GetPrivateProfileIntA("Modes",   "DiagMissionWatch", s.DiagMissionWatch, p);
    s.RecapFreeze      = GetPrivateProfileIntA("Modes",   "RecapFreeze",      s.RecapFreeze,      p);

    // An unvalidated id indexes the screen-slot array (mainMgr + 0x1E8 + id*8).
    // slot(311) lands exactly on mainMgr+0xBA0, so 310 is the hard ceiling.
    if (s.ResultScreenId < 0 || s.ResultScreenId > 310) {
        hoe::Log("config: ResultScreenId=%d out of range 0-310, using 225", s.ResultScreenId);
        s.ResultScreenId = 225;
    }
    // sub_3A46D0 silently does nothing (leaves the draw gate shut) unless the
    // variant is 0..3, which is exactly why the ctor's 4 sentinel renders nothing.
    if (s.ResultVariant < 0 || s.ResultVariant > 3) {
        hoe::Log("config: ResultVariant=%d out of range 0-3, using 0", s.ResultVariant);
        s.ResultVariant = 0;
    }
    if (s.ResultHoldFrames < 1 || s.ResultHoldFrames > 3600) {
        hoe::Log("config: ResultHoldFrames=%d out of range, using 180", s.ResultHoldFrames);
        s.ResultHoldFrames = 180;
    }
    // Guards against ResultTimeoutSec * 60 overflowing.
    if (s.ResultTimeoutSec < 0 || s.ResultTimeoutSec > 3600) {
        hoe::Log("config: ResultTimeoutSec=%d out of range 0-3600, using 120", s.ResultTimeoutSec);
        s.ResultTimeoutSec = 120;
    }

    hoe::Log("config: Enabled=%d ShowResultScreen=%d ResultScreenId=%d ResultTimeoutSec=%d "
             "TrackBestScore=%d DiagDumpFields=%d UnlockAllModes=%d",
             s.Enabled, s.ShowResultScreen, s.ResultScreenId, s.ResultTimeoutSec,
             s.TrackBestScore, s.DiagDumpFields, s.UnlockAllModes);
    return s;
}

}  // namespace

const Settings& Get() { static Settings s = Load(); return s; }

}  // namespace config
