# Experimental Skyward Sword HD Link asset conversion

`convert-sshd-link.py` converts a narrow SSHD asset layout: a big-endian
BRRES containing MDL0 revision 11 models with the same model names and bone order
as the Wii Link, plus external textures in `g3d/model.xtx` inside `Alink.arc`.
It rejects different rigs and texture counts. The XTX image order must match the
BRRES placeholder order; matching counts alone cannot prove an unknown mod's
mapping. Inspect the exported texture previews and test the result in game.

This is **not general Switch mod support**. BFRES models, changed rigs, Switch
executable patches, randomizer logic and other HD archives need separate work.
The DBZ Future Trunks package was tested in Skyloft and Skyview using copied
saves, movement, camera and both Link outfit variants. Cutscenes, facial states
and all later equipment combinations have not been exhaustively tested.

Use Python 3.11+ with Pillow, numpy and texture2ddecoder 1.0.6 installed:

```powershell
python -m pip install Pillow numpy texture2ddecoder==1.0.6
python tools/convert-sshd-link.py --alink "C:/mods/HD/Alink.arc" --game-dir "C:/your/extracted-Wii-game" --out "trunks-hd" --id "trunks-hd" --name "Future Trunks" --author "Original mod author"
```

The input is the inner HD `Alink.arc`, not a RAR, a complete HD ObjectPack or an
FBX. The output folder must be new and outside the extracted SOUE01 game.
Optional `--decoder-dir` adds an existing Python dependency folder. No Python
or decoding libraries are required to play the completed mod.

The tool decodes supported ASTC/BC1/BC3 XTX images and creates compact CMPR or
RGB5A3 Wii fallback textures at original Wii sizes. It also writes the complete
HD images as Dolphin-hash PNG replacements, independent of SkyBreak's AI pack
switch. It appends new TEX0 resources while preserving model, bone, material and
animation bytes. LZ11 compression reduces the loading footprint. Some Toolbox
exports have stale mip metadata; the converter reports this and reads the base
image from the bounded actual data block. Mipmaps are not imported.

`conversion.json` records texture mappings and hashes. `author-textures/` holds
previews; `textures/` contains the runtime replacements. Enable only one complete
Link/ObjectPack mod at a time and restart SkyBreak. A successful conversion still
needs game testing: archive size, resource references and rendering can fail.
Keep the generated ObjectPack local because it includes assets from your game.
Share your original assets and conversion instructions instead.

Format references: [Switch-Toolbox XTX reader](https://github.com/KillzXGaming/Switch-Toolbox/blob/master/File_Format_Library/FileFormats/Texture/XTX.cs),
[Tegra block-linear addressing](https://gist.github.com/aboood40091/13e574c050a03b820b130452fb8fbaf2).
The optional [texture2ddecoder](https://pypi.org/project/texture2ddecoder/)
dependency is MIT licensed and installed separately; it is not bundled here.
