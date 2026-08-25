# Reference image

`Yakuza4_dumped.bin` is a **decrypted** dump of the `Yakuza4.exe` module, taken from a running
process. It is ~58 MB and is **not committed** (see ../.gitignore).

Why it exists: `Yakuza4.exe` ships with SteamStub DRM, so `.text` on disk is encrypted
(entropy 8.000). Static analysis and the offline tests need the decrypted form.

Key property: **file offset == RVA**. The dump starts at the module base and preserves the
virtual layout, so `img[0xB5C3C0]` is the byte at RVA 0xB5C3C0.

## Regenerating

1. Launch Yakuza 4 and get to any in-game state.
2. Run `tools/dumpmod.ps1 -ProcName Yakuza4 -OutDir refs` (from the scratchpad toolset).
3. Confirm it is decrypted: `.text` entropy should be ~5.9, not 8.0, and the region at
   offset 0x1000 should contain plenty of `CC` padding bytes.

The base the dump was taken at is recorded by the tool (`IMAGEBASE_RUNTIME`). The offline tests
assume `0x7FF6F6D00000`; if you re-dump at a different base, update `_find_vtable` in
`tests/test_offsets.py`.

# SLLZ fixture

`pjs_net_ranking.csb.sllz` is the raw, still-compressed `pjs_net_ranking.csb` member lifted
out of `data/2d/cse_en.par`. It is game data and is **not committed** (see ../.gitignore).
`tests/test_sllz.py` skips without it.

Why it exists: the SLLZ v1 bitstream has a trap that makes a wrong decoder look right on
short inputs. The flag byte is refilled *after the eighth bit is consumed but before that
eighth token's payload is read*, so the next flag byte physically precedes the last token of
the previous group. Refilling at the top of the loop - the obvious implementation -
desynchronises at the first group boundary and only fails partway in. This 26 KB member
expands to 188 KB, which is long enough that any such decoder fails the round trip.

## Regenerating

```sh
python - "<game>/data/2d/cse_en.par" refs/pjs_net_ranking.csb.sllz <<'PY'
import sys, par                       # tools/ must be on sys.path
arc = par.ParArchive(sys.argv[1])
f = next(f for f in arc.files if f.name == "pjs_net_ranking.csb")
open(sys.argv[2], "wb").write(par._read_payload(arc, f))
PY
```
