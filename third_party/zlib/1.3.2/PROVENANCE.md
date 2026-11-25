# zlib 1.3.2 provenance

- Upstream project: https://www.zlib.net/
- Upstream release: https://github.com/madler/zlib/releases/tag/v1.3.2
- Source archive: `artifacts/zlib-1.3.2.tar.gz`
- Published/retrieved SHA-256: `BB329A0A2CD0274D05519D61C667C062E06990D72E125EE2DFA8DE64F0119D16`
- Release date reported upstream: 2026-02-17
- SFX use: statically linked DEFLATE support for the pinned libarchive build.

The source archive is retained byte-for-byte. The build extracts it only into
the generated build tree, verifies the SHA-256 before extraction, and disables
the shared library, examples/tests, and installation outside that generated
tree.

The upstream license is copied verbatim in `LICENSE.txt`.
