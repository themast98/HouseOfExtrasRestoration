#pragma once
namespace config {
struct Settings {
    int Enabled          = 1;
    int ShowResultScreen = 0;   // no local result screen is correct for these modes;
                                // the original was an ONLINE leaderboard (see config.cpp)
    int ResultScreenId   = 225; // 225 = CActionTougijyoAllStarResult, what our sibling macro opens
    int ResultVariant    = 0;   // 0..3 - which result page of the layout to show
    int ResultHoldFrames = 180; // frames the screen holds before playing its "out" anim
    int TrackBestScore   = 1;   // CActionColosseumExtra+0x208, confirmed 3/4/6
    int DiagDumpFields   = 0;   // dump candidate offsets to the log
    int ResultTimeoutSec = 120; // seconds; 0 = wait indefinitely
    int DiagScreenState  = 1;   // log the result screen's own state each second
    int CallTransitionSetup = 1; // mirror the reference handler's fade call
    int ForceFadeIn      = 0;   // after opening, fade the screen back IN (test aid)
    int UnlockAllModes   = 1;   // show all of Bob's rows, not just Battle King
    int DiagMissionWatch = 1;   // log the running macro + step; names any hang
    int RecapFreeze      = 0;   // hold the recap on screen and open a command channel
    int NetRankingPanel  = 1;   // rebuild the real recap on the pjs_net_ranking layout
};
const Settings& Get();   // reads HouseOfExtras.ini once, writes defaults if absent
}
