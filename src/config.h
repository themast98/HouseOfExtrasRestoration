#pragma once
namespace config {
struct Settings {
    int Enabled          = 1;
    int ShowResultScreen = 1;   // 0 = skip straight to the exit step
    int ResultScreenId   = 225; // 225 = pjs_dlc_result (the real recap screen)
    int TrackBestScore   = 1;
    int ResultTimeoutSec = 0;   // 0 = wait indefinitely, like vanilla
};
const Settings& Get();   // reads HouseOfExtras.ini once, writes defaults if absent
}
