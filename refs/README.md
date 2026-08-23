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
