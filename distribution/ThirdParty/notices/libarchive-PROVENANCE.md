# libarchive 3.8.9 provenance

- Upstream project: https://www.libarchive.org/
- Upstream release: https://github.com/libarchive/libarchive/releases/tag/v3.8.9
- Source archive: `artifacts/libarchive-3.8.9.zip`
- Published/retrieved SHA-256: `7B20200203D08866B62373DD51417798427EC67852928D71F142BFE578E9EA75`
- Release date reported upstream: 2026-07-28
- SFX use: statically linked ZIP/Zip64 creation and inspection through the
  libarchive API. Upstream command-line programs are disabled and are not
  packaged.

The source archive is retained byte-for-byte. The build extracts it only into
the generated build tree, verifies the SHA-256 before extraction, disables all
optional compression and parsing dependencies except the separately pinned
zlib 1.3.2 source, and builds the static library only.

The license summary copied from the source distribution is in `LICENSE.txt`.
Individual source-file notices in the upstream archive remain controlling.
