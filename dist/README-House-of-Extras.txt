House of Extras Restoration 1.0.0 - Yakuza 4 Remastered (PC, Steam)
by Themast

Restores Bob Utsunomiya's House of Extras to how it played on PS3: all seven
modes selectable and replayable, the NETWORK RANKING result panels, the kill
counter and Endless Survival counter, the Ameba coliseum floor in Battle King /
Fastest Killer, Speed King's intro on every run, and Bob's post-mode lines.

REQUIREMENTS
  - Yakuza 4 Remastered on Steam (Yakuza4.exe from the 2024-07-21 build).
  - Shin Ryu Mod Manager (SRMM) with its Parless loader. Parless is what loads
    this mod's plugin (HouseOfExtras.asi) from inside the mod folder.
  - A save in Premium Adventure (the House of Extras is a post-game feature).

INSTALL
  1. Install and run Shin Ryu Mod Manager once so that the game folder contains
     YakuzaParless.asi and a "mods" folder.
  2. Copy the "House of Extras Restoration" folder from this archive into
     the game's "mods" folder, so that you have:
        Yakuza 4\mods\House of Extras Restoration\mod-meta.yaml
        Yakuza 4\mods\House of Extras Restoration\HouseOfExtras.asi
        Yakuza 4\mods\House of Extras Restoration\2d\...
        Yakuza 4\mods\House of Extras Restoration\stage\...
        Yakuza 4\mods\House of Extras Restoration\wdr_par_en\...
        Yakuza 4\mods\House of Extras Restoration\font_hd\...
     (Nothing goes in the game folder itself.)
  3. Open Shin Ryu Mod Manager, tick "House of Extras Restoration", and click
     Save / Rebuild MLO. This is required: Parless only loads the plugin once
     the mod is listed in YakuzaParless.mlo.
  4. Launch the game. HouseOfExtras.ini, HouseOfExtras.log and
     HouseOfExtras.records are created inside the mod folder on the first run.

UPGRADING FROM A PRE-RELEASE BUILD
  Earlier test builds put HouseOfExtras.asi (and its .ini / .log / .records)
  next to Yakuza4.exe. Delete those from the game folder, or move the .ini and
  .records into the mod folder to keep your settings and best scores. Two copies
  of the plugin must never be loaded at once.

UNINSTALL
  Untick the mod in Shin Ryu Mod Manager and rebuild the MLO, then delete the
  "House of Extras Restoration" folder. Nothing else is left behind.

NOTES
  - Both halves are needed: the .asi restores the deleted code paths, the mod
    folder restores the deleted data (textures, layouts, Bob's talk script).
    Disabling the mod in SRMM disables both.
  - HouseOfExtras.ini exposes every fix as a switch. The defaults are the PS3
    behaviour; the Diag* keys turn on logging and are off.
  - FixChaseDrainRate corrects the stamina drain in EVERY chase (story chases
    included) to the PS3 rate. Set it to 0 in the ini for vanilla PC pacing.
  - Only the English game files are covered (wdr_par_en, cse_en).
