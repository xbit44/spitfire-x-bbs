# Moebius CP437 font provenance

The Control Panel uses the unmodified 4,096-byte IBM CP437 bitmap font
`moebius-cp437.f16`. Its SHA-256 is
`ff3640ffffdd774ec56ee7ecacf7bce8324a67071155ade07f4c6380fa879025`.

The exact upstream member is `app/fonts/ibm/CP437.F16` in Moebius 1.0.29,
commit `5df8344ecfad24863386d86fd5a5f494616036f8`:

- Font: <https://raw.githubusercontent.com/blocktronics/moebius/5df8344ecfad24863386d86fd5a5f494616036f8/app/fonts/ibm/CP437.F16>
- License: <https://raw.githubusercontent.com/blocktronics/moebius/5df8344ecfad24863386d86fd5a5f494616036f8/LICENSE.txt>
- Upstream credits: <https://raw.githubusercontent.com/blocktronics/moebius/5df8344ecfad24863386d86fd5a5f494616036f8/README.md>

The font matches that upstream member byte for byte. The release supplies an
Apache License 2.0 text and a README identifying Copyright 2020 Andy Herbert
and the project's Apache-2.0 terms. The complete inspected release tree had
no separate IBM-font license or exclusion. SFX retains the release's published
project-level licensing basis, original license text and applicable upstream
credits; it makes no modification to the font or claim of new authorship.

The matching notice files are included in source and runtime distributions:

- `distribution/ThirdParty/notices/Moebius-CP437-NOTICE.txt`
- `distribution/ThirdParty/notices/Moebius-LICENSE.txt`
- `distribution/ThirdParty/notices/Moebius-UPSTREAM-README.md`

Their runtime destinations are under `Docs/ThirdParty/notices/`. The original
license is 11,358 bytes, SHA-256
`cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`;
the upstream README is 3,595 bytes, SHA-256
`2b06e38a289a59baba0b5f6e60700440672bbd59d80d1e919c7846099b9f6d7a`.

This November 15, 2026 provenance update supersedes the earlier cache-limited
private-candidate wording. Exact release/font/license evidence and earlier
reports remain preserved in the project's private readiness records.

The separate captured Windows VGA font `sfx-vga-cp437.fon` is obsolete. It is
excluded from the beta.8 Control Panel and corresponding source package; its
historical working-tree file and frozen earlier packages remain preserved.
Moebius's license is not used as a license for that separate captured font.
