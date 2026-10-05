# First public test release checklist

Keep every item unchecked until evidence supports it. A documentation-only starter can be published as documentation; it must not be tagged or advertised as a playable PC port.

## Scope and evidence

- [ ] Freeze the source commit and identify the tested native executable.
- [ ] Record actual platform support and tested hardware/toolchain versions.
- [ ] Demonstrate the advertised gameplay scope and document progression blockers.
- [ ] Update README and status with evidence; remove obsolete starter wording.
- [ ] Confirm exact accepted game revision and user-data format.

## Installation and behavior

- [ ] Verify clean-machine setup using tested instructions.
- [ ] Verify missing-data and wrong-revision errors without crashes or misleading prompts.
- [ ] Verify controllers and motion-control mappings for the advertised scope.
- [ ] Verify audio, scene changes, repeated startup, and save/load where supported.
- [ ] Document limitations and include useful, sanitized troubleshooting steps.

## Content and provenance

- [ ] Inspect complete Git history and LFS, not only the current working tree.
- [ ] Inspect release archive, binaries/resources, installer, CI artifacts, and caches for proprietary game data and secrets.
- [ ] Confirm no ISO/WBFS/RVZ/WAD, original DOL/REL, extracted assets, embedded original data, or game-data download links are distributed.
- [ ] Confirm installer and application require user-supplied files and never fetch game content.
- [ ] Preserve dependency/upstream notices; resolve source license scope.
- [ ] Put the no-assets and own-copy requirement in README, release notes, and installation instructions.

## Package and announcement

- [ ] Test the exact archive that will be uploaded.
- [ ] Generate checksums and record artifact names.
- [ ] Prepare release notes naming tested features, requirements, limitations, and source commit.
- [ ] Label the release as an experimental test build if that is its actual status.
- [ ] Review any community links and remove unconfirmed affiliations or promises.
- [ ] Obtain the maintainer's explicit authorization before externally publishing this prepared release.
