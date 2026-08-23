#pragma once
namespace config {
struct Settings {
    int Enabled          = 1;
    int ShowResultScreen = 1;   // 0 = skip straight to the exit step
    int ResultScreenId   = 220; // 220 = pjs_dlc_survivalbtl_end
    int TrackBestScore   = 1;
};
const Settings& Get();   // reads HouseOfExtras.ini once, writes defaults if absent
}
