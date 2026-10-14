# SkyBreak asset mods, schema 1

Open **SkyBreak > Mods > Open mods folder**, extract each mod into its own
subfolder, choose **Refresh**, then enable it. New mods are disabled by default.
The top mod wins conflicts; **Up/Down** changes priority. The same screen is
available under **F1 > Mods**. Changes during gameplay require restarting SkyBreak.

Target: **Skyward Sword Wii USA 1.0, SOUE01**. This supports compatible Wii game
asset/archive replacements and Dolphin-hash PNG/DDS texture replacements. Texture
mods are independent of the built-in HD/AI pack switch. The source game and saves
are unchanged. To recover from a broken mod, disable it in the launcher or use
**SkyBreak-zonder-mods.cmd**.

```text
mods/example/mod.toml
mods/example/textures/tex1_256x128_08eaf82df408426e_14.png
mods/example/files/Object/ObjectPack.arc.LZ
```

Either asset folder may be omitted, but the mod needs at least one replacement.

```toml
schema = 1
id = "example"
name = "My Link mod"
game = "SOUE01"
version = "1.0"
author = "Your name"
description = "A custom look for Link."
```

IDs must be unique lowercase ASCII letters/digits/`-`/`_`, at most 64 characters.
Selection and order are saved in `mods/state.toml`.

Textures require original Dolphin hash names, a base PNG/DDS and compatible
aspect/alpha. Higher resolutions, `_m`, `_arb` and companion `_mipN` images work.
Use complete hashes for predictable priority; wildcard versus exact-hash matches
retain the existing texture loader's matching rules. Both mip-filter variants of
an exact texture receive the mod's priority. Some indirect/watercolor atlases
remain protected by the renderer. Selected controller glyphs have separate
priority over button pictures.

For Link, prepare a **compatible Wii `Alink.arc`** containing BRRES resources
and preserving required bones, animation bindings and resource names. Raw FBX,
GLB and Switch HD models cannot be loaded directly. Link lives inside
`Object/ObjectPack.arc.LZ` as `oarc/Alink.arc`. From the mods folder, run:

```powershell
python "tools/make-link-mod.py" --game-dir "C:/your/extracted-game" --alink "C:/your/Alink.arc" --out "my-link" --id "my-link" --name "My Link model"
```

Requires Python 3.11+. The game folder must contain `sys/boot.bin` and
`files/Object/ObjectPack.arc.LZ`; the output folder must not exist. The helper
checks archive structure, preserves other game assets, and writes a valid LZ11
archive. Rig/material compatibility still requires testing. Literal compression
produces a larger file and can add loading time. The resulting ObjectPack
includes original game assets: keep it local; share your own model/new assets
and instructions for users to build from their own game.

Alternatively, use `--brres path/to/model.brres` instead of `--alink` to build
Alink.arc from an edited compatible BRRES. `--extract --out new-folder` extracts
the original Alink.arc and model.brres as an editing starter. Archive structure
checks do not validate rig/material compatibility.

Whole-file replacements are not merged. Two ObjectPack replacements conflict;
one complete archive wins. Version 1 has no ZIP import, downloads, code plugins
or live model reloading. Unknown disc paths are reported; executable/system
replacements, invalid manifests, wrong game IDs and paths escaping the mod's
folder are rejected.

Some Skyward Sword HD Link assets can first be converted with
`tools/convert-sshd-link.py`. See [SSHD-conversion.md](SSHD-conversion.md) for the
tested BRRES+XTX layout, dependencies and limits. This does not enable general
Switch mods or executable patches.

Developers: `NWII_MODS_DIR` overrides the root; `NWII_MODS=0` skips all mods.
Virtual FST relocation supports replacement size changes without overlapping
original files. `[Mods]` log lines identify mounts, overrides and unknown paths.
