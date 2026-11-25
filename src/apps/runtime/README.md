# Runtime application

Alpha 0.0.1 provides the Desktop Mode `SFXRuntime.exe`. It is launched and owned by the sibling `SFXControlPanel.exe`, accepts only that authenticated local control relationship, owns the protected SQLite runtime, and brings up the configured Telnet/SSH listeners after bootstrap. Stage 3 separates both acceptors from dedicated node workers sharing one four-slot pool and one SSH host identity. Independent persisted node controls do not recreate listeners. Cancel-all precedes join-all, status remains authoritative during shutdown, and failed durable cleanup faults/quarantines rather than permitting reuse.

Stage 4 binds that original authenticated owner to matched private protocol 3.0 and one process-wide Runtime epoch. Complete status snapshots are monotonically ordered and active screens carry exact epoch/node/generation ownership. Existing startup fallback retains generation high-water marks and cannot replace a quarantined service. Owner exit or control loss still shuts all callers down; no replacement-owner/handoff capability is added.

The Alpha Runtime is not a Windows Service and is not a standalone command-line program. See [`docs/ALPHA_0.0.1.md`](../../../docs/ALPHA_0.0.1.md) for build and run instructions.
