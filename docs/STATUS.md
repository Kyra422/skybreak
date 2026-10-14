# Status and evidence

Current evidence, 2026-10-14: [Alpha 3](RELEASE_0.23.0_ALPHA3.md) is published with
Windows installer and input/motion regressions passing. The newer
[Wii Remote preview](WII_REMOTE_PREVIEW.md) built and loaded a private save;
physical controller testing is pending. [Online co-op](ONLINE_COOP_RESEARCH.md)
has an inactive player-context prototype and is not playable.

The table below is the original 2026-10-05 starter audit. Consult the linked
milestone reports for later verification and its limits.

The original starter table did not assess native playability.

| Area | Recorded status | Evidence needed |
|---|---|---|
| Native configure/build/launch | Not assessed | Commit, toolchain, commands, OS/architecture, output |
| Loading user-supplied resources | Not assessed | Accepted format, local validation, missing-data handling |
| Rendering | Not assessed | Test scene, backend, limitations, reproducible result |
| Input and motion-control translation | Not assessed | Devices, mappings, test cases |
| Audio | Not assessed | Playback and synchronization checks |
| Scenes and gameplay | Not assessed | Tested section and known failures |
| Save/load | Not assessed | Round-trip and compatibility tests |
| PC platform support | Not assessed | Per-platform build and runtime results |
| Release packaging | Not assessed | Archive review and complete no-assets audit |

## Evidence entry format

For each milestone record: date, exact commit, native/Wii/emulator target, environment, commands or reproduction steps, result, and limitations. Use `planned`, `in progress`, `partially verified`, `verified`, or `blocked` once supported by evidence.

A successful compilation proves compilation only. Upstream matching progress does not measure native playability. Replace the public README's status paragraph when actual evidence supports a more specific claim.
