# Building Skybreak

This starter has no native runtime source or verified native build procedure. Build instructions will be added from the actual implementation.

Before documenting a supported platform, verify:

- OS and architecture; compiler, SDK, build-system, and dependency versions.
- Dependency fetch locations and pinned revisions.
- Exact configure, build, and launch commands from a clean checkout.
- Which output is a native PC executable and where it is placed.
- User-provided data format, revision validation, and local file location.
- No-data startup, incorrect-data failure, and one reproducible successful run.
- Known limitations and checks that were not run.

The upstream research target is Wii `SOUE01`. Its reconstruction uses original local `sys/main.dol` and `files/rels.arc`; those two inputs are not a promised complete asset set for the native port. Never publish them or substitute upstream `configure.py`/`ninja` steps for unverified Skybreak PC instructions.
