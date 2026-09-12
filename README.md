# House of Extras Restoration for Yakuza 4 Remastered (PC)

Restores Bob Utsunomiya's House of Extras to how it played on PS3: all seven modes
selectable and replayable, the NETWORK RANKING result panels, the kill counter and
Endless Survival counter, the Ameba coliseum floor in Battle King / Fastest Killer,
Speed King's intro on every run, and Bob's post-mode lines.

This repository holds the **plugin source** (`HouseOfExtras.asi`), its offline
tests and the reverse-engineering tools. The **release package** (plugin plus the
restored game data) is distributed separately; the data files are modified Sega
assets and are not part of this repository.

- **How it works**: [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)
- **Install instructions for players**: [dist/README-House-of-Extras.txt](dist/README-House-of-Extras.txt)

## Requirements

- Yakuza 4 Remastered on Steam (Yakuza4.exe from the 2024-07-21 build).
- [Shin Ryu Mod Manager](https://github.com/SRMM-Studio/ShinRyuModManager) with its
  Parless loader. Parless loads the plugin from inside the mod folder; nothing goes
  in the game root.

## Building

The plugin is a plain native DLL with no dependencies beyond kernel32 and the
Universal C Runtime.

- [mingw-w64 GCC](https://winlibs.com/) (UCRT build) on `PATH`, or set `GXX` to the
  compiler path.
- Python 3 for the offset-header generator and the tests (`pip install pytest`).

```bash
bash build.sh
```

produces `build/HouseOfExtras.asi`. Copy it into
`<game>/mods/House of Extras Restoration/` next to `mod-meta.yaml`, or run

```bash
bash tools/deploy.sh
```

which builds, runs the tests and copies the plugin into the installed mod folder
(set `GAME` if the game is not in the default Steam library path). It refuses to
run while the game is open and if a stray `HouseOfExtras.asi` exists in the game
root, because two loaded copies would fight over the same patches.

## Tests

```bash
python -m pytest tests -q
```

`tests/test_offsets.py` verifies every address and patch site against a decrypted
memory dump of the executable. The dump is game code and is **not committed**; the
tests skip without it. `refs/README.md` explains how to produce it from your own
copy of the game.

## Layout

```
src/          plugin source (see docs/HOW-IT-WORKS.md, section 11)
offsets.json  the offset table; tools/gen_offsets.py turns it into src/offsets.h
tests/        offline verification
tools/        build/deploy scripts and reverse-engineering helpers
docs/         design notes and the architecture document
refs/         reference data produced from your own game copy (not committed)
dist/         player-facing README; release packages are built here (not committed)
```

## Principles

- No hardcoded addresses outside `resolve.cpp`. Everything is found at runtime from
  patterns and RTTI and checked against an expected value; a resolve failure
  installs nothing.
- Every fix is an ini switch whose default is the PS3 behaviour.
- Bytes are verified before they are written. Patches are reversible.
- Diagnostics are disabled, never deleted.

## Credits

- Themast: research, design and implementation.
- SRMM-Studio for Shin Ryu Mod Manager and Parless.
- The Like A Brawler team, whose released code showed the patch and settings
  patterns this project started from.

## License

MIT, see [LICENSE](LICENSE). The license covers this repository's source only.
Yakuza 4 and its assets remain the property of Sega.
