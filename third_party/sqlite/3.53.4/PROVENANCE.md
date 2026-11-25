# SQLite 3.53.4 provenance

- **Component:** SQLite 3.53.4 canonical source and amalgamation
- **Purpose and owning adapter:** Private embedded storage engine for `sfx_storage_sqlite`; never exposed as a public or dynamic database ABI
- **Upstream project:** SQLite
- **Authoritative location:** <https://www.sqlite.org/download.html>
- **Release record:** <https://www.sqlite.org/releaselog/3_53_4.html>
- **Pinned version:** 3.53.4, release 2026-07-24
- **Source ID:** `2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc`

## Exact artifacts

| File | Bytes | Official SHA3-256 | Measured SHA-256 |
|---|---:|---|---|
| `artifacts/sqlite-src-3530400.zip` | 14,557,315 | `b834d474b9b393d85a9e3ee4cc11f1329e007e9376a424ee740796f5c4bda3a8` | `D18FA15AEC74D8C17E1463F861095ADC01B5AD190256ACB4F91D22F0368D232B` |
| `artifacts/sqlite-amalgamation-3530400.zip` | 2,946,650 | `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e` | `1E71DDF93849C6A6ECF58B827C0692073D2DD7EE40196158068F7B29F422E87D` |
| `sqlite3.c` | 9,515,341 | `67f423e9ebbbdc473cbc4772c872ee6b89f31fde4ed0279a5c25d5f65c043a16` | `B1DD5D74EC7F29055A6684FA06FB3C2F6821C87DD38F9A458DFD2E8A1DB28189` |
| `sqlite3.h` | 690,838 | — | `919E7F2E8ED1D8F56AC17B412B8971C76AA5D1A879752CC6058F75E7D5910E1D` |
| `LICENSE.md` | 3,850 | — | `EE6AF51062B30D532991FACE5164136AE6F84E265ECF8ABE89DC69DAC45CA1E7` |

`sqlite3.c`, `sqlite3.h`, and `sqlite3ext.h` are byte-for-byte copies from the pinned amalgamation archive. The canonical source archive is retained for provenance and upstream-suite qualification.

## License and notices

SQLite identifies the primary source, tests, extensions, amalgamation, command-line shell, and listed utility code as public domain. The exact upstream `LICENSE.md` is retained beside the build input. No upstream patch is applied.

## Build and isolation

- Static private linkage only; the SQLite target is not exported.
- MSVC builds inherit the repository's static CRT policy and Win32/x86 requirement.
- Windows compilation retains the Windows 7 API and subsystem floor.
- Linux GCC and Clang builds are portability verification only.
- Final compile definitions are maintained once in the root `CMakeLists.txt` and checked by storage qualification tests.
- Final definitions: SQLITE_THREADSAFE=1, SQLITE_DQS=0, SQLITE_OMIT_LOAD_EXTENSION, SQLITE_OMIT_SHARED_CACHE, SQLITE_ENABLE_API_ARMOR, SQLITE_SECURE_DELETE=1, and SQLITE_LIKE_DOESNT_MATCH_BLOBS.
- Foreign keys and trusted-schema restrictions are explicitly enforced and verified per SFX connection rather than changing upstream compile defaults required by the canonical test suite.
- Extension loading and shared-cache support are omitted. Foreign keys, API armor, defensive connections, trusted-schema restrictions, strict validation, WAL/full synchronization, and local-path enforcement are applied by the adapter.

## Update policy

Review SQLite releases monthly and published critical security/corruption fixes within 72 hours. Version changes require a newly pinned official artifact, hashes and license evidence, full rebuild, upstream-suite run under SFX compile options, adapter/fault tests, Win32/x86 PE/import review, Windows 7/10/11 execution, and Linux GCC/Clang verification. Release builds perform no network dependency resolution.

## Approval

Owner/design-room approved SQLite 3.53.4 and the exact official amalgamation for Stage 5 on 2026-08-23, subject to the required Stage 5A runtime qualification gates.
