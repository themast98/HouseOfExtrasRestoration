# DEPRECATED - delegates to shot.py. Do not restore the old implementation.
#
# This script used PrintWindow with PW_RENDERFULLCONTENT and fell back to
# Graphics.CopyFromScreen. Both are GDI, and GDI cannot see this game's D3D
# swap chain. Every capture returned only the ~7,600 pixels of the RTSS overlay
# lit and the other two million black - which is indistinguishable from a
# genuinely black frame, so "0 lit pixels" was read as evidence the panel was
# not drawing when in fact the instrument was blind.
#
# The clincher: a capture taken in Naomi's Palace - a fully lit 3D scene with
# the restored panel plainly visible on screen - also scored 0 lit pixels.
#
# shot.py uses DXGI Desktop Duplication, which captures the composited desktop
# and therefore sees exactly what the player sees.

$py = Join-Path $PSScriptRoot "shot.py"
& python $py @args
exit $LASTEXITCODE
