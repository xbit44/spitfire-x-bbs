# Embedded Control Panel visual assets

The byte-authored IBM CP437 ANSI artwork was supplied by the project owner for the
Alpha 0.0.1 Slice B Control Panel correction pass. Production embeds the current ANSI
artwork as read-only resources and performs bounded runtime cell substitutions after
parsing. This table also preserves the identities of historical review assets; it is
not a list of every resource included in the current program.

| Resource | Bytes | SHA-256 |
| --- | ---: | --- |
| `sfx-node-view.ans` | 1,089 | `EA46192455E1A66C43EE0B6561307B34C5E783B998BC24D04D510429FC7EB76C` |
| `sf-x-ss.ans` | 1,363 | `E5B72650C3678A5E5D62BD094890CDB35172EA0E4CC01EEF2DD069804098A7A6` |
| `sfx-about-placeholder.png` | 627,724 | `60BF6ABB0AE4738F7A051B59882B50ACF68BF0CF221F6F1048D5040660359788` |
| `sfx-vga-cp437.fon` | 5,168 | `011326F65E151D83C157EF6FDD23A2D851E8D0F2662D8409EA0B4A142E343A13` |

The ANSI and PNG source provenance is the owner Design Room packet dated 2026-09-02.
The repository copies preserve the supplied bytes, including the ANSI SAUCE records.

The historical placeholder PNG remains preserved but is no longer embedded as of
v0.0.2-alpha R1. Its native dimensions are 622x620 pixels.

## Owner-approved R1 branding (2026-09-09)

The exact masters are copied from the Design Room's locked branding packet; no
redrawing or substitution is performed. About embeds the original PNG bytes.

| Current asset | Native size | SHA-256 |
| --- | --- | --- |
| `sfx-about-approved.png` | 1254x1254 | `19AFE23E9D598EF058BE271F15783A6692FD74DEB3A6EA3E79E9C6A78130041C` |
| `sfx-control-panel-icon-approved.png` | 1254x1254 RGBA | `1D1255945B46255CDCFCB43098FE4BC6878C6B83EBCAE59F73B39C891A4C7295` |
| `sfx-control-panel-icon.ico` | 16,24,32,48,64,128,256 | `DF38D30BE9690AAC40F4C4BCAFD175C6462FDAE3A75C23B0224514931380F3F6` |

The Setup master and ICO are stored at `src/apps/setup/`:

| Asset | SHA-256 |
| --- | --- |
| `sfx-installer-icon-approved.png` | `882817393E76EB5188057C6A72218B21F0C759D3815ED4BB3A524E0CB4B95295` |
| `sfx-installer-icon.ico` | `84FCC559E20BE8FFBA37A2F20588AA41536FB9DCA3BA7CE5329ED41EEFCC05DB` |

`test/tools/alpha_002_r1_icons.py` generates 32-bit Windows DIB/AND-mask ICO
entries mechanically from those exact masters. Small entries use bounded area
coverage, hard alpha and a quantized gray palette; the source orange eye is
retained at small size. Larger entries preserve nearest-neighbor pixel edges.
The icon preview permits explicit 16/24/32-pixel eye/horn/badge inspection. The
approved compact head, not the complete About composition, is the executable
identity. No custom UAC shield is added.

The current Control Panel terminal canvas retains the unmodified Moebius CP437.F16
bitmap resource, `moebius-cp437.f16`, SHA-256
`FF3640FFFFDD774EC56EE7ECACF7BCE8324A67071155ADE07F4C6380FA879025`.
Its exact upstream release, published Apache-2.0 basis, original license and credits
are documented in [MOEBIUS_FONT_PROVENANCE.md](MOEBIUS_FONT_PROVENANCE.md).

The separately captured Windows VGA OEM face, `sfx-vga-cp437.fon`, is obsolete and
excluded from the beta.8 program and corresponding source package. Its historical
working-tree asset and earlier frozen review evidence remain unchanged. Removing
that dependency does not change SFXConfig's existing system-font behavior.
