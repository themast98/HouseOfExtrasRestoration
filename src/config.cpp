#include "config.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace config {
namespace {

// The ini lives next to HouseOfExtras.asi inside the mod folder (see paths.h).
// GetPrivateProfile* resolves bare names against the Windows directory, so it
// has to be an absolute path.
const char* IniPath() { return hoe::ModRelative("HouseOfExtras.ini"); }

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
        ";  WHY THE DEFAULT IS 0, for now. None of the engine's surviving result\n"
        ";  screens is the right one: 220 is a caption banner, 222 belongs to the\n"
        ";  survival-battle flow and 225 to the Coliseum tournament. Opening any\n"
        ";  of them gives a healthy object with nothing in it.\n"
        ";\n"
        ";  The real panel is pjs_net_ranking, drawn by the deleted\n"
        ";  d:/Project/Soul_oe/src/ranking/ranking.cpp. Video of the PS3 build\n"
        ";  running OFFLINE shows it renders perfectly well with no server:\n"
        ";      NETWORK RANKING\n"
        ";      -Battle King Ranking (Kiryu)-\n"
        ";      Score / Latest Record  0 / Best Record  0\n"
        ";  It is a small LOCAL panel - your latest and your best - not a\n"
        ";  leaderboard of other players. The network only supplied the numbers.\n"
        ";\n"
        ";  Everything visual survives on PC: the title, the labels and the\n"
        ";  artwork are baked into pjs_net_ranking.par / _hires.par, which ship\n"
        ";  inside data/2d/cse_*.par. Only three short dynamic strings were lost -\n"
        ";  the board name and the two numbers - and this mod already tracks\n"
        ";  latest and best locally. So this is genuinely restorable.\n"
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
        ";  0 = wait indefinitely, which is now the default AND what the engine's\n"
        ";  own handler does - it polls the screen slot with no clock at all.\n"
        ";  120 was a guard from when the panel had no dismiss handling and could\n"
        ";  strand the game. The panel has a Close prompt and a confirm button now,\n"
        ";  so a clock would only cut the recap short while you were reading it.\n"
        ";  Set a number here if you want the old bounded behaviour back.\n"
        "ResultTimeoutSec=0\n"
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
        "DiagScreenState=0\n"
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
        "\n;  1 = rebuild the end-of-mode panel on the engine's own pjs_net_ranking\n"
        ";  layout - the one the deleted src/ranking code drew.\n"
        ";\n"
        ";  CURRENTLY 0, BECAUSE IT CRASHES. The layout itself loads fine, but the\n"
        ";  surviving widget used to bind it was written for a DIFFERENT layout\n"
        ";  (pjs_mg_net_ranking) and walks that layout's pane structure unchecked:\n"
        ";  after fetching page 0 it does [pane+0x10] -> [that+0x20] -> byte[+5]\n"
        ";  with no null test anywhere along the chain.\n"
        ";  Do not set this to 1 until netrank.cpp stops calling NETRANK_BIND on a\n"
        ";  foreign layout. The mode ends cleanly with this off.\n"
        "NetRankingPanel=0\n"
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
        "DiagMissionWatch=0\n"
        "\n;  1 = hold the recap on screen indefinitely and poll HouseOfExtras.cmd\n"
        ";  for commands, replying in HouseOfExtras.reply.\n"
        ";  A development aid: it makes the recap inspectable and experimentable\n"
        ";  live, instead of needing a fresh play session for every attempt.\n"
        ";  Send `resume` to let it finish normally. Leave at 0 for play.\n"
        "RecapFreeze=0\n"
        "\n;  Frames before the panel dismisses ITSELF without a button press.\n"
        ";  0 = never, the default: hold until the player closes it, like the\n"
        ";  engine's own handler. It is a backstop for two things a pad read cannot\n"
        ";  guarantee - that no engine site cleared the pad edges earlier in the\n"
        ";  same frame, and that the pad manager exists at all. If a confirm press\n"
        ";  is ever missed with this at 0 the panel holds forever, so 3600 (~60s)\n"
        ";  is the value to restore if that happens.\n"
        "DismissWatchdog=0\n"
        "ResetExtrasFlags=1\n"
        "DiagFlagsOnly=0\n"
        "\n;  Bob's post-mode line (\"What, done already?\" / \"Did you enjoy yourself?\").\n"
        ";  PC never sets the two flags his talk entries test; 1 = set them at\n"
        ";  mode end the way PS3's mode code does. 0 = leave them alone.\n"
        "PostModeBobFlags=1\n"
        "\n;  Ameba collaboration decal on the coliseum floor in Battle King / Fastest\n"
        ";  Killer, as on PS3. PC's coliseum scene code clears the scene flag that\n"
        ";  shows it; 1 = raise it again while a ranking coliseum mode is armed.\n"
        "AmebaFloor=1\n"
        "DiagMissionTrace=0\n"
        "PanelDirectDraw=1\n", f);
    fclose(f);
}

Settings Load() {
    Settings s;
    const char* p = IniPath();
    if (!p) { hoe::Log("config: could not build ini path, using built-in defaults"); return s; }

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
    s.DiagStartup      = GetPrivateProfileIntA("Modes",   "DiagStartup",      s.DiagStartup,      p);
    s.RecapFreeze      = GetPrivateProfileIntA("Modes",   "RecapFreeze",      s.RecapFreeze,      p);
    s.DismissWatchdog  = GetPrivateProfileIntA("Modes",   "DismissWatchdog",  s.DismissWatchdog,  p);
    s.ResetExtrasFlags = GetPrivateProfileIntA("Modes",   "ResetExtrasFlags", s.ResetExtrasFlags, p);
    s.DiagFlagsOnly    = GetPrivateProfileIntA("Modes",   "DiagFlagsOnly",    s.DiagFlagsOnly,    p);
    s.DiagMissionTrace = GetPrivateProfileIntA("Modes",   "DiagMissionTrace", s.DiagMissionTrace, p);
    s.PanelDirectDraw  = GetPrivateProfileIntA("Modes",   "PanelDirectDraw",  s.PanelDirectDraw,  p);
    s.PanelSortBias    = GetPrivateProfileIntA("Modes",   "PanelSortBias",    s.PanelSortBias,    p);
    s.PanelTextPrio    = GetPrivateProfileIntA("Modes",   "PanelTextPrio",    s.PanelTextPrio,    p);
    s.PanelNumbers     = GetPrivateProfileIntA("Modes",   "PanelNumbers",     s.PanelNumbers,     p);
    s.PanelPulseLatest = GetPrivateProfileIntA("Modes",   "PanelPulseLatest", s.PanelPulseLatest, p);
    s.PanelRollFrames  = GetPrivateProfileIntA("Modes",   "PanelRollFrames",  s.PanelRollFrames,  p);
    s.PanelRecordBanner= GetPrivateProfileIntA("Modes",   "PanelRecordBanner",s.PanelRecordBanner,p);
    s.PanelBannerFrames= GetPrivateProfileIntA("Modes",   "PanelBannerFrames",s.PanelBannerFrames,p);
    s.PanelNumberWidth = GetPrivateProfileIntA("Modes",   "PanelNumberWidth", s.PanelNumberWidth, p);
    s.PanelNumberFont  = GetPrivateProfileIntA("Modes",   "PanelNumberFont",  s.PanelNumberFont,  p);
    s.PanelSounds      = GetPrivateProfileIntA("Modes",   "PanelSounds",      s.PanelSounds,      p);
    s.PanelSeRoll      = GetPrivateProfileIntA("Modes",   "PanelSeRoll",      s.PanelSeRoll,      p);
    s.PanelSeSettle    = GetPrivateProfileIntA("Modes",   "PanelSeSettle",    s.PanelSeSettle,    p);
    s.PanelSeNewRecord = GetPrivateProfileIntA("Modes",   "PanelSeNewRecord", s.PanelSeNewRecord, p);
    s.PanelSeDecide    = GetPrivateProfileIntA("Modes",   "PanelSeDecide",    s.PanelSeDecide,    p);
    s.PanelRankingFont = GetPrivateProfileIntA("Modes",   "PanelRankingFont", s.PanelRankingFont, p);
    s.ChaseRankPanel   = GetPrivateProfileIntA("Modes",   "ChaseRankPanel",   s.ChaseRankPanel,   p);
    s.ChaseWinCode     = GetPrivateProfileIntA("Modes",   "ChaseWinCode",     s.ChaseWinCode,     p);
    s.EndlessTagPanel  = GetPrivateProfileIntA("Modes",   "EndlessTagPanel",  s.EndlessTagPanel,  p);
    s.DiagForceEndlessRecord =
        GetPrivateProfileIntA("Modes", "DiagForceEndlessRecord", s.DiagForceEndlessRecord, p);
    s.EndlessSuppressResult =
        GetPrivateProfileIntA("Modes", "EndlessSuppressResult", s.EndlessSuppressResult, p);
    s.FixEndlessHandoff= GetPrivateProfileIntA("Modes",   "FixEndlessHandoff",s.FixEndlessHandoff,p);
    s.FixChaseDrainRate= GetPrivateProfileIntA("Modes",   "FixChaseDrainRate",s.FixChaseDrainRate,p);
    s.ChaseReplayIntro = GetPrivateProfileIntA("Modes", "ChaseReplayIntro", s.ChaseReplayIntro, p);
    s.EscapeReplayIntro = GetPrivateProfileIntA("Modes", "EscapeReplayIntro", s.EscapeReplayIntro, p);
    s.DiagEscapeTrace  = GetPrivateProfileIntA("Modes", "DiagEscapeTrace",  s.DiagEscapeTrace,  p);
    s.DiagCaptionTrace = GetPrivateProfileIntA("Modes", "DiagCaptionTrace", s.DiagCaptionTrace, p);
    s.DiagTalkMatch    = GetPrivateProfileIntA("Modes", "DiagTalkMatch",    s.DiagTalkMatch,    p);
    s.PostModeBobFlags = GetPrivateProfileIntA("Modes", "PostModeBobFlags", s.PostModeBobFlags, p);
    s.AmebaFloor       = GetPrivateProfileIntA("Modes", "AmebaFloor",       s.AmebaFloor,       p);
    s.ChaseResetCaptionSeen = GetPrivateProfileIntA("Modes", "ChaseResetCaptionSeen", s.ChaseResetCaptionSeen, p);
    s.DiagDrainClock   = GetPrivateProfileIntA("Modes",   "DiagDrainClock",   s.DiagDrainClock,   p);
    s.ChaseDrainFramesPer10 =
        GetPrivateProfileIntA("Modes", "ChaseDrainFramesPer10", s.ChaseDrainFramesPer10, p);
    s.FixSteamAchievementCrash =
        GetPrivateProfileIntA("Modes", "FixSteamAchievementCrash", s.FixSteamAchievementCrash, p);
    s.FixTagHelpPanel  = GetPrivateProfileIntA("Modes",   "FixTagHelpPanel",  s.FixTagHelpPanel,  p);
    s.PanelNumberCellW = GetPrivateProfileIntA("Modes",   "PanelNumberCellW", s.PanelNumberCellW, p);
    s.PanelNumberMetric= GetPrivateProfileIntA("Modes",   "PanelNumberMetric",s.PanelNumberMetric,p);
    s.PanelPulseDelayMs= GetPrivateProfileIntA("Modes",   "PanelPulseDelayMs",s.PanelPulseDelayMs,p);
    s.PanelPulseHoldMs = GetPrivateProfileIntA("Modes",   "PanelPulseHoldMs", s.PanelPulseHoldMs, p);
    s.DiagTextBind     = GetPrivateProfileIntA("Modes",   "DiagTextBind",     s.DiagTextBind,     p);
    s.PanelHideTimeLabel = GetPrivateProfileIntA("Modes", "PanelHideTimeLabel", s.PanelHideTimeLabel, p);
    s.PanelClosePrompt   = GetPrivateProfileIntA("Modes", "PanelClosePrompt",   s.PanelClosePrompt,   p);
    s.NetRankingPanel  = GetPrivateProfileIntA("Results", "NetRankingPanel",  s.NetRankingPanel,  p);

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
