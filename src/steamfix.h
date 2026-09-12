#pragma once

// Works around a crash in the STOCK game that has nothing to do with this mod.
//
// Yakuza4.exe's Steam achievement callback formats each achievement's display
// name and description with snprintf("%s", ...). The compiler inlined a
// page-UNSAFE SSE4.2 strlen at 147 sites in the binary: it issues unaligned
// 16-byte vmovdqu loads with no alignment prologue, so it reads past the end of
// a committed page whenever the string starts within 15 bytes of the boundary -
// even when that string is perfectly NUL-terminated. Steam hands back
// "Saejima the Slinger" at page offset 0xFE2 (30 bytes of slack), the second
// 16-byte load spans 0xFF2..0x1001, and the process dies.
//
// PROVEN NOT OURS: reproduced 6/6 with the entire modding stack removed
// (no HouseOfExtras.asi, no YakuzaParless.asi, no dinput8.dll proxy, no mods
// folder - WER's module list confirms), and once on 2026-08-23 two minutes
// before this project's folder was created.
//
// The fix redirects the two achievement call sites to a bounded byte-wise copy.
// Single-byte reads cannot straddle a page, so the fault is impossible by
// construction, and the copy also refuses to walk into a page that is not
// committed. See InstallSteamAchievementFix for the safety checks.
bool InstallSteamAchievementFix();
