# Roadmap and proposed milestones

No milestone is marked complete by this starter. These are acceptance criteria, not deadlines or feature promises. Create the milestones manually only when the repository is approved.

| Milestone | Suggested title | Completion criteria |
|---|---|---|
| M0 | Foundations | Record source baseline, architecture decision, source provenance, and applicable licenses; document the boundary between Wii reconstruction and PC implementation. |
| M1 | First native launch | Build and launch a native PC executable from documented commands on one named platform; no emulator invocation; record commit and tool versions. |
| M2 | Bring your own game | Validate a supported local game revision, load a real required resource, reject missing/incorrect data clearly, and keep all game files outside distribution. |
| M3 | First scene | Demonstrate rendering, camera/input, and the required audio path for a defined scene; describe every placeholder and unsupported control. |
| M4 | Playable slice | Complete a bounded gameplay sequence with scene transitions and tested control mappings; verify save/load where the slice requires it. |
| M5 | Broader adventure | Track scene coverage, progression blockers, stability, save/load correctness, and repeatable regression checks. |
| M6 | Public test build | Pass the first-release checklist; provide tested setup instructions, named platform support, known limitations, and an asset-free package. |

## Possible later improvements

Alternative controls, accessibility options, display settings, performance work, modding, and additional platforms can be evaluated after the foundation works. Higher frame rates require explicit simulation and timing validation. None of these is advertised as implemented.

## Suggested first issues

- Document the native platform boundary: produce an architecture note listing Wii interfaces and proposed replacements.
- Verify the build on one PC platform: record exact commands and executable behavior.
- Specify user-data import: document required revision, format, validation, and error behavior.
- Audit source provenance and packaging: record notices and data exclusions before publishing source or binaries.

Use one issue per bounded result. Attach evidence before closing a milestone.
