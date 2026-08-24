#pragma once
namespace config {
struct Settings {
    int Enabled          = 1;
    int ShowResultScreen = 1;   // 0 = skip straight to the exit step
    int ResultScreenId   = 225; // 225 = pjs_dlc_result (the real recap screen)
    int TrackBestScore   = 0;   // off: the score field is not yet identified
    int DiagDumpFields   = 0;   // dump candidate offsets to the log
    int ResultTimeoutSec = 120; // seconds; 0 = wait indefinitely
    int DiagScreenState  = 1;   // log the result screen's own state each second
    int CallTransitionSetup = 1; // mirror the reference handler's fade call
};
const Settings& Get();   // reads HouseOfExtras.ini once, writes defaults if absent
}
