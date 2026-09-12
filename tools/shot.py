"""Capture the Yakuza 4 window to a PNG.

WHY THIS EXISTS (and why shot.ps1 is not enough)

shot.ps1 used PrintWindow, then fell back to Graphics.CopyFromScreen. Both are
GDI, and GDI cannot see this game's D3D swap chain: every capture came back with
only the ~7,600 pixels of the RTSS overlay lit and the other two million black.
That is indistinguishable from a genuinely black frame, and it produced two
separate wrong conclusions in one session - including a capture of Naomi's
Palace, a fully lit 3D scene, scored as "0 lit pixels".

DXGI Desktop Duplication captures the composited desktop, which includes
D3D and exclusive-fullscreen output, so it sees what the player sees.

Usage:
    python shot.py [out.png] [--region L T R B] [--full]

Exits non-zero and prints ERR ... on failure, so callers can tell a failed
capture from a black frame - the distinction that caused all the trouble.
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

GAME_CLASS = "RINO4_APPLICATION_CLASS"

user32 = ctypes.WinDLL("user32", use_last_error=True)


def _find_game_window():
    """Return (hwnd, (l, t, r, b)) for the game's client area in screen pixels.

    Matched by window CLASS. Yakuza4.exe also owns a ConsoleWindowClass window
    for YakuzaParless, and picking by MainWindowHandle or by "largest area"
    both chose that console at least once, silently turning a screenshot into
    960x480 of scrollback.
    """
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)   # per-monitor aware
    except Exception:
        user32.SetProcessDPIAware()

    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == GAME_CLASS and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    if not found:
        return None, None

    hwnd = found[0]
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)          # SW_RESTORE
        time.sleep(0.5)

    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    origin = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))
    return hwnd, (origin.x, origin.y,
                  origin.x + rect.right, origin.y + rect.bottom)


def main():
    args = [a for a in sys.argv[1:]]
    out = "yakuza_shot.png"
    region = None
    full = False
    i = 0
    while i < len(args):
        if args[i] == "--region":
            region = tuple(int(x) for x in args[i + 1:i + 5])
            i += 5
        elif args[i] == "--full":
            full = True
            i += 1
        else:
            out = args[i]
            i += 1

    hwnd, client = _find_game_window()
    if hwnd is None and not full:
        print("ERR Yakuza 4 window not found (class %s)" % GAME_CLASS)
        return 2
    if region is None:
        region = None if full else client

    import dxcam
    cam = dxcam.create(output_color="RGB")
    if cam is None:
        print("ERR dxcam could not open a desktop duplication device")
        return 3

    # Duplication only hands back a frame when the desktop actually changed, so
    # a single grab can legitimately return None on a static screen. Retry
    # briefly rather than reporting a black frame.
    frame = None
    for _ in range(40):
        frame = cam.grab(region=region) if region else cam.grab()
        if frame is not None:
            break
        time.sleep(0.05)
    cam.release()

    if frame is None:
        print("ERR no frame from desktop duplication (screen may be static; "
              "this is NOT the same as a black frame)")
        return 4

    from PIL import Image
    im = Image.fromarray(frame)
    im.save(out)
    # Reported so a caller can tell "captured a dark frame" from "captured
    # nothing"; numpy rather than getdata() to stay quiet and fast.
    import numpy as np
    lit = int((frame.astype(np.uint16).sum(axis=2) > 24).sum())
    print("OK %s (%dx%d, dxgi-duplication, lit=%d/%d)"
          % (out, im.size[0], im.size[1], lit, im.size[0] * im.size[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
