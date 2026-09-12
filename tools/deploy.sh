#!/bin/sh
# Build, verify, and deploy - in that order, refusing to ship a stale binary.
#
# WHY THIS EXISTS. A broken build once reached the game folder because the
# deploy step read only the tail of build.sh's output, which on failure is a
# compiler diagnostic rather than the "built:" banner. The unit tests did not
# catch it either: they validate offsets against the memory dump and never load
# the .asi, so they pass just as happily when compilation failed. The result was
# a "deployed" message, green tests, and the PREVIOUS build still in place.
#
# Three guards, each covering a different way that went wrong:
#   1. build.sh's exit status is checked (set -e alone was not enough, because
#      the failure was being swallowed by a pipeline).
#   2. the output must actually contain the "built:" banner.
#   3. the deployed file must end up NEWER than every source file.
set -e
cd "$(dirname "$0")/.."

GAME="${GAME:-/d/SteamLibrary/steamapps/common/Yakuza 4}"
# The .asi ships INSIDE the mod folder: Parless loads every .asi that SRMM lists
# for an enabled mod (Parless.cpp InitializeScripts), so nothing goes in the
# game root any more. A stray root copy would be loaded a second time by the
# ASI loader and fight over the same patches, hence the refusal below.
MODDIR="$GAME/mods/House of Extras Restoration"
ASI="$MODDIR/HouseOfExtras.asi"

if tasklist 2>/dev/null | grep -qi yakuza4; then
    echo "REFUSING: Yakuza4.exe is running - a loaded .asi is locked on disk."
    exit 1
fi

out=$(bash build.sh 2>&1) || { echo "$out" | grep -iE "error" | head -20; \
    echo "BUILD FAILED - nothing deployed."; exit 1; }
echo "$out" | grep -q "^built:" || { echo "$out" | tail -20; \
    echo "BUILD produced no binary - nothing deployed."; exit 1; }
echo "$out" | grep "^built:"

python -m pytest tests -q >/dev/null || { echo "TESTS FAILED - nothing deployed."; exit 1; }
echo "tests: pass"

if [ -e "$GAME/HouseOfExtras.asi" ]; then
    echo "REFUSING: $GAME/HouseOfExtras.asi still exists - the .asi now lives in the mod folder; move it out of the game root first."
    exit 1
fi
[ -d "$MODDIR" ] || { echo "REFUSING: mod folder missing: $MODDIR"; exit 1; }
cp build/HouseOfExtras.asi "$ASI"

# The binary must be newer than every source, or something is stale.
newest=$(ls -t src/*.cpp src/*.h offsets.json 2>/dev/null | head -1)
if [ "$newest" -nt "$ASI" ]; then
    echo "REFUSING: $newest is newer than the deployed binary - stale build."
    exit 1
fi
echo "deployed $(stat -c%s "$ASI") bytes to $MODDIR"
