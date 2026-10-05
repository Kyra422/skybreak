# Contributing to Skybreak

Thanks for helping bring Skyward Sword to PC. Small, understandable changes are easier to review and test.

## Before you start

Read the [status](docs/STATUS.md), [roadmap](docs/ROADMAP.md), and [asset policy](docs/ASSETS_AND_LICENSING.md). For a large change, open a proposal before spending time on the implementation. Search existing issues first.

## What to contribute

Useful early work includes documenting the native platform boundary, verifying toolchain requirements, designing local resource loading, and producing reproducible rendering or input experiments. These are suggested areas, not confirmed implementations or assigned tasks.

## Pull requests

1. Work on a branch and keep each PR focused on one problem.
2. Follow the conventions of the affected code. Avoid unrelated formatting changes.
3. Explain the resulting behavior and any remaining limitations.
4. Record the commit, OS, architecture, tools, and exact checks used. Say explicitly when a check was not run.
5. Preserve upstream authorship, license notices, and dependency attribution.
6. Review the staged diff and attachments for game data, secrets, and personal paths.

You must have permission to submit the material you contribute. Do not claim ownership of upstream code or remove its notices. Dependencies need provenance and compatible terms before integration. Licensing of the eventual port must be resolved from the actual source and dependencies; this starter grants no blanket license over them.

## Game files stay local

Skybreak does not release game assets. You need your own legally obtained copy of Skyward Sword to supply original game data. Never contribute disc images, original DOL/REL binaries, extracted textures, models, music, dialogue, keys, or archives containing game data. Do not provide links to unauthorized copies.

Tests must use original synthetic fixtures or clearly redistributable material. Do not attach saves or memory dumps containing proprietary data; use sanitized descriptions and logs instead.

## Issues and testing

Use the issue templates. Include expected behavior, observed behavior, steps, and a build identifier. State whether you tested a native executable, a Wii rebuild, or an emulator session. Decompilation matching percentages are not native-port completion percentages.

Automated tools can assist your work, but you remain responsible for understanding, provenance, correctness, and validation of everything you submit. No separate AI contribution ban is established by this starter.
