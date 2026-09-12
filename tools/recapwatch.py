"""Watch the mod's log and screenshot the recap the moment it opens.

WHY THIS EXISTS
Every panel experiment so far has ended with "play a run and tell me what you
see". That makes the human the measuring instrument, and it costs a full arena
run per hypothesis. Two things fix that:

  * the mod now logs the engine's own emitted-quad counter around our draw, so
    "did it render" is a number rather than a judgement;
  * this script captures the frame itself, so nobody has to describe it.

It tails the log, and when the panel opens it captures via DXGI Desktop
Duplication (NOT GDI - see tools/shot.py for why GDI silently returns black on
this game's swap chain) and reports lit-pixel statistics next to the quad count.

Usage:
    python recapwatch.py [--timeout 900] [--outdir DIR]

Exits 0 when it captured, 1 on timeout, 2 on capture failure - so the caller can
tell "nothing happened" from "the capture broke", which is exactly the confusion
that produced two wrong conclusions earlier in this project.
"""

import argparse
import os
import re
import subprocess
import sys
import time

GAME_DIR = r"D:\SteamLibrary\steamapps\common\Yakuza 4\mods\House of Extras Restoration"  # runtime files live next to the .asi
LOG = os.path.join(GAME_DIR, "HouseOfExtras.log")
SHOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shot.py")

# The panel is up once this appears; the quad line follows within a frame or two.
OPEN_RE = re.compile(r"netrank: (built after|page 0 unsuppressed)")
QUAD_RE = re.compile(r"draw2d: drew (\d+) element\(s\).*?-> (\d+) quad")
DISMISS_RE = re.compile(r"panel dismissed")


def tail(path, from_end=True):
    """Yield appended lines; tolerates the file being rotated out from under us."""
    while not os.path.exists(path):
        time.sleep(0.5)
    f = open(path, "r", encoding="utf-8", errors="replace")
    if from_end:
        f.seek(0, os.SEEK_END)
    while True:
        line = f.readline()
        if line:
            yield line.rstrip("\n")
        else:
            time.sleep(0.15)
            # rotated? reopen
            try:
                if os.stat(path).st_size < f.tell():
                    f.close()
                    f = open(path, "r", encoding="utf-8", errors="replace")
            except OSError:
                pass


def capture(out):
    r = subprocess.run([sys.executable, SHOT, out, "--full"],
                       capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def stats(png):
    try:
        from PIL import Image
    except ImportError:
        return "(PIL missing)"
    im = Image.open(png).convert("L")
    px = list(im.getdata())
    lit = sum(1 for v in px if v > 12)
    return "%dx%d  lit=%d/%d (%.2f%%)  max=%d  mean=%.1f" % (
        im.width, im.height, lit, len(px), 100.0 * lit / len(px), max(px),
        sum(px) / len(px))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--outdir", default=os.path.dirname(os.path.abspath(__file__)))
    a = ap.parse_args()

    print("watching %s (timeout %.0fs)" % (LOG, a.timeout), flush=True)
    deadline = time.time() + a.timeout
    armed = False
    shots = 0
    quad_lines = []

    for line in tail(LOG):
        if time.time() > deadline:
            print("TIMEOUT - the recap never opened", flush=True)
            return 1

        if QUAD_RE.search(line):
            quad_lines.append(line)
            print("QUAD  " + line, flush=True)

        if not armed and OPEN_RE.search(line):
            armed = True
            print("PANEL OPEN: " + line, flush=True)
            # let it build and draw a few frames before looking
            time.sleep(1.5)
            for shot_i in range(3):
                out = os.path.join(a.outdir, "recap_%d.png" % shot_i)
                rc, msg = capture(out)
                if rc != 0:
                    print("CAPTURE FAILED: %s" % msg, flush=True)
                    return 2
                print("SHOT %s  %s" % (out, stats(out)), flush=True)
                shots += 1
                time.sleep(1.0)

        if armed and DISMISS_RE.search(line):
            print("panel dismissed; %d shot(s) taken" % shots, flush=True)
            for q in quad_lines[-4:]:
                print("  " + q, flush=True)
            return 0 if shots else 1

    return 1


if __name__ == "__main__":
    sys.exit(main())
