#pragma once
namespace config {
struct Settings {
    int Enabled          = 1;
    int ShowResultScreen = 1;   // 0 = skip straight to the exit step
    int ResultScreenId   = 220; // 220 = pjs_dlc_survivalbtl_end (the Extras results screen)
    int ResultVariant    = 0;   // 0..3 - which result page of the layout to show
    int ResultHoldFrames = 180; // frames the screen holds before playing its "out" anim
    int TrackBestScore   = 0;   // off: the score field is not yet identified
    int DiagDumpFields   = 0;   // dump candidate offsets to the log
    int ResultTimeoutSec = 120; // seconds; 0 = wait indefinitely
    int DiagScreenState  = 1;   // log the result screen's own state each second
    int CallTransitionSetup = 1; // mirror the reference handler's fade call
};
const Settings& Get();   // reads HouseOfExtras.ini once, writes defaults if absent
}
