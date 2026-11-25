# curl 8.22.0

Unmodified upstream source, release 2026-09-02, retrieved 2026-09-23 over
certificate-verified HTTPS from https://curl.se/download/curl-8.22.0.tar.xz.
SHA-256: `F7EF3AE8A22E521F289803FE93543EB64C329B58AA73A9E224DFD915A2A5F4F7`.
This is the recorded intake identity; no independent signature verification is
claimed. Archive includes complete COPYING (curl permissive license), shipped
with the owner package. Source is preserved offline and hash checked by CMake.

One build-local integration patch in `cmake/SfxBotGateTLS.cmake` replaces
`keylog_file_name = getenv("SSLKEYLOGFILE");` with a NULL assignment in
`lib/vtls/keylog.c`. This disables creation/writing of optional TLS secret logs,
including when the process inherits that debugging environment variable.
No certificate-validation or TLS-protocol code is changed. Upstream source
archive stays exact. A changed upstream patch context fails configuration.
curl's published 8.22.0 vulnerability page listed no published vulnerabilities
at intake: https://curl.se/docs/vuln-8.22.0.html; not a guarantee of absence.

Build: MSVC v143 Win32 static /MT, Windows 7 target, HTTP-only libcurl with
OpenSSL 3.5.8. No curl executable, DLL, proxy, native CA store, CA filesystem
fallback, auth, cookies, netrc, HSTS, alt-svc or OpenSSL configuration autoload.
HTTP is compiled for HTTPS framing but the application permits HTTPS URLs only.
Threaded resolver; 3-second connect, 30-second whole transfer, 3-second stalled
transfer, cancellation and 16-MiB size bound. Not an inbound TLS service.
Upstream INSTALL.md supports Vista/Server 2008 minimum; that is a compatibility
claim, not evidence of our genuine Windows 7/10 execution.
