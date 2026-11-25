# Third-Party Components

Only approved external libraries/components may be placed here.

For every component, record:

- exact name, source location, version/commit, and upstream project;
- copyright, license, notices, attribution, and redistribution obligations;
- source/binary provenance and integrity information;
- dependencies and build instructions;
- Win32 32-bit, Windows 7, and future Linux impact;
- security/maintenance status;
- isolation/API boundary and reason for inclusion;
- approval record.

Do not place unreviewed third-party source or binaries here. Presence of historical third-party material under read-only `reference\` is not approval to reuse or redistribute it.

Synchronet-derived/adapted code additionally requires the completed audit described in `docs\SYNC_REUSE_AUDIT.md`. SFX is not a Synchronet fork.

Current authorized production dependency intake:

- `libsodium/1.0.22`: exact official 1.0.22 source, built unmodified as a private static dependency for Stage 5C; ISC notice and complete provenance are retained beside the source.
- `seclists/2026.1`: exact reviewed Strong-mode 10,000-password resource and MIT notice; it is hash-verified before use and has no runtime updater.

The exact hashes, build/linkage policy, qualification evidence, and update requirements are recorded in `research/security/STAGE5C_DEPENDENCY_PROVENANCE.md`.
