# Status and evidence

Documentation prepared: 2026-10-05. Actual native implementation inspected: none during preparation of this starter.

The previous setup helper and repository launch materials do not prove a working native port. Unknown status does not mean no work has been done; it means evidence is still needed here.

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
