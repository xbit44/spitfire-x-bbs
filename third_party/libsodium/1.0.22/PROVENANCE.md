# libsodium 1.0.22 provenance

- Upstream: <https://github.com/jedisct1/libsodium> and <https://download.libsodium.org/libsodium/releases/>
- Release: `1.0.22-RELEASE`, published 2026-04-09
- Official source artifact: `artifacts/libsodium-1.0.22.tar.gz`
- Source artifact SHA-256: `ADBDD8F16149E81AC6078A03ACA6FC03B592B89EF7B5ED83841C086191BE3349`
- Official MSVC artifact: `artifacts/libsodium-1.0.22-msvc.zip`
- MSVC artifact SHA-256: `3E03A726FAC4BC09CB61D8F29D658EF7A5ECA0811DE59082130414F7CA2E4279`
- Selected official library: `msvc/Win32/Release/v143/static/libsodium.lib`
- Selected library SHA-256: `DF308911390228FC4EAEC04FE05712453604113C1EB733FBA1F45F694BEA4E12`
- Selected primary header SHA-256: `29D5D70A432320FB40BC1234302F2BF59BA2E78C5E61494B894D35B18E0FEACF`
- License: ISC; exact `source/LICENSE` SHA-256 `508A76D186356C0DD807A670EF510964F8724557024796A2C426C6C0E19AB683`
- Copyright: Copyright (c) 2013-2026 Frank Denis `<j at pureftpd dot org>`
- Windows linkage: official unmodified v143 Win32 Release static library, `SODIUM_STATIC`, private SFX adapter, static CRT `/MT`
- Linux companion linkage: unmodified vendored source configured `--disable-shared --enable-static --enable-minimal --disable-dependency-tracking` with the selected C compiler; this is portability verification, not Linux-native product work
- Update policy: reviewed point-release intake only; record new artifacts/hashes/license/security disposition and rerun the complete upstream, SFX, and Windows 7–11 qualification matrices

SFX uses only the private password-hashing, random-byte, constant-time comparison,
SHA-256, and sensitive-memory primitives required by Stage 5C. Availability of
other APIs does not authorize additional cryptographic or product functionality.
