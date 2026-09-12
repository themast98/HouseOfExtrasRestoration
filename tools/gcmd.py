"""Send commands to the frozen recap and read the reply.

The ASI polls HouseOfExtras.cmd from the game thread roughly four times a
second while the recap is frozen, executes each line, and writes
HouseOfExtras.reply. This wraps that handshake so an experiment is one call.

Usage:
    python gcmd.py "base"
    python gcmd.py "readrva 1980D18 8" "slots"
    python gcmd.py --file commands.txt
"""

import argparse
import pathlib
import sys
import time

GAME = pathlib.Path(r"D:\SteamLibrary\steamapps\common\Yakuza 4") / "mods" / "House of Extras Restoration"  # runtime files live next to the .asi
CMD = GAME / "HouseOfExtras.cmd"
REPLY = GAME / "HouseOfExtras.reply"


def send(lines, timeout=15.0):
    # A stale reply from a previous command would look like a fresh answer, so
    # clear it and wait for the file to be recreated.
    try:
        REPLY.unlink()
    except FileNotFoundError:
        pass

    CMD.write_text("\n".join(lines) + "\n", encoding="ascii")

    deadline = time.time() + timeout
    while time.time() < deadline:
        if REPLY.exists():
            time.sleep(0.15)          # let the write finish
            try:
                text = REPLY.read_text(encoding="ascii", errors="replace")
            except OSError:
                time.sleep(0.1)
                continue
            if text.strip():
                return text
        time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*")
    ap.add_argument("--file")
    ap.add_argument("--timeout", type=float, default=15.0)
    a = ap.parse_args()

    lines = list(a.commands)
    if a.file:
        lines += [l for l in pathlib.Path(a.file).read_text().splitlines() if l.strip()]
    if not lines:
        ap.error("no commands given")

    out = send(lines, a.timeout)
    if out is None:
        print("TIMEOUT - no reply. Is the game running with the recap frozen "
              "(RecapFreeze=1, and actually sitting on the recap)?", file=sys.stderr)
        sys.exit(1)
    print(out)


if __name__ == "__main__":
    main()
