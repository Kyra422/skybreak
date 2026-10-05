<div align="center">
  <h1>SKYBREAK</h1>
  <p><strong>A new horizon for Skyward Sword.</strong></p>
  <p>An independent project working toward a native PC port of<br>
  <em>The Legend of Zelda: Skyward Sword</em> for Wii.</p>
  <p>
    <a href="docs/STATUS.md">Project status</a> •
    <a href="docs/ROADMAP.md">Roadmap</a> •
    <a href="CONTRIBUTING.md">Contributing</a>
  </p>
</div>

> [!IMPORTANT]
> **Skybreak does not distribute game assets or copies of the game. You must own a legally obtained copy of Skyward Sword and supply the required game files from your own copy to play Skybreak when playable builds become available.** No textures, models, music, dialogue, original game executables, disc images, or other extracted game data are provided.

# Overview

Skybreak aims to bring the original Wii adventure to PC through a native port, while preserving the feel of the game that started the legend.

The starting point is the reverse-engineering work of [ZeldaRET's Skyward Sword decompilation](https://github.com/zeldaret/ss). Rebuilding the Wii game and making it run natively on PC are separate milestones. Skybreak's goal is a native PC executable; the decompilation alone does not provide one.

# Project status

**Early development.** This repository starter documents the project's direction. Native gameplay, supported PC platforms, and release readiness have not been independently verified for this starter.

See the [status page](docs/STATUS.md) for the evidence record and the [roadmap](docs/ROADMAP.md) for planned milestones. Planned improvements are goals, not currently supported features. There is no announced release date.

# Setup

> [!IMPORTANT]
> **Bring your own game.** Skybreak will not provide assets, game dumps, download links to copyrighted game files, or a complete copy of Skyward Sword. Owning the game is required; this repository is not a substitute for buying it.

### 1. Prepare your own copy

The initial decompilation baseline is **Wii NTSC-U 1.0 (USA), `SOUE01`**. This identifies the starting research target, not verified native-port compatibility. Support for other revisions and Skyward Sword HD is not claimed.

Use files extracted from your own legally obtained copy, in accordance with the rules that apply where you live. Keep those files on your own machine. Never upload them to this repository, an issue, a pull request, or a release.

### 2. Run Skybreak

Native installation steps, system requirements, and the accepted game-data format will be documented after they have been tested. This starter contains documentation and repository templates, not a playable port.

# Building

The native toolchain and build instructions still need to be verified against the actual implementation. See [Building](docs/BUILDING.md) for what must be documented before a reproducible build is announced.

The upstream decompilation's Wii build commands must not be presented as Skybreak PC build instructions.

# Contributing

Help with platform research, reproducible build documentation, resource loading, rendering, input, and testing is welcome. Read [CONTRIBUTING](CONTRIBUTING.md) before opening an issue or pull request.

Keep changes focused, explain how they were tested, and never include original game data. Bugs should identify the affected commit or release and provide steps that others can reproduce using their own game files.

# Credits

Thanks to the [ZeldaRET Skyward Sword decompilation team](https://github.com/zeldaret/ss) and the wider GameCube/Wii reverse-engineering community for the work that makes this project possible.

See [CREDITS](CREDITS.md) for provenance and [the asset and licensing policy](docs/ASSETS_AND_LICENSING.md) for distribution rules.

Skybreak is an unofficial, independent project. It is not affiliated with, sponsored by, or endorsed by Nintendo, ZeldaRET, or the Dusklight team. The Legend of Zelda and Skyward Sword names and original game content belong to their respective rights holders.
