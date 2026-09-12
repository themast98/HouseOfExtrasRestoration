#!/bin/sh
# Builds HouseOfExtras.asi with mingw-w64 GCC (WinLibs UCRT).
# The game must be CLOSED - a loaded .asi is locked on disk.
set -e
# Compiler: g++ from PATH, or set GXX to a full path, e.g. the WinLibs install
# under %LOCALAPPDATA%/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.*/mingw64/bin/g++.exe
GXX="${GXX:-g++}"
cd "$(dirname "$0")"
python tools/gen_offsets.py
mkdir -p build
"$GXX" -shared -O2 -std=c++17 -static -Isrc \
    -o build/HouseOfExtras.asi \
    src/dllmain.cpp src/log.cpp src/paths.cpp src/console.cpp src/netrank.cpp src/patch.cpp src/resolve.cpp src/config.cpp src/game.cpp src/recap.cpp src/records.cpp src/probe.cpp src/input.cpp src/steamfix.cpp src/talkdiag.cpp src/ameba.cpp \
    -Wl,--kill-at
echo "built: build/HouseOfExtras.asi ($(stat -c%s build/HouseOfExtras.asi) bytes)"
