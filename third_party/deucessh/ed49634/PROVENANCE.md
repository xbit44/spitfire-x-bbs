# DeuceSSH source intake

Owner-authorized private SSH integration; no publication authorization is implied.

- Upstream: https://github.com/RealDeuce/DeuceSSH
- Exact commit: `ed49634ce5dbab8c236313358076f325da28fb5e`.
- Archive URL: https://github.com/RealDeuce/DeuceSSH/archive/ed49634ce5dbab8c236313358076f325da28fb5e.zip
- Local artifact: `artifacts/DeuceSSH-ed49634.zip`, 790,819 bytes.
- SHA-256: `04AE1F08D17D221CEFAFF2EE67530F56D84F97307B64822C6192B22508FC828A`.
- The complete 1,755-line README was read before integration code was written. Its exact preserved copy is `UPSTREAM_README.md`, 59,642 bytes, SHA-256 `A7E85FAE1FF7DAE78EE0726E421C7D40FA40C42D27FD700A5105A2C85CE40D34`.
- The archive and its extracted original source are unmodified. CMake verifies the archive and creates a separate explicitly patched build-tree overlay as described below, then builds statically against the unchanged private OpenSSL 3.5.8 libcrypto target.
- Upstream uses C17 and MSVC C11-atomics support privately. SFX production remains C++17. Its supplied Windows thread shim is selected by the native-thread capability probe. Both dependency and product target Win32/x86, Windows 7 API level, static CRT.
- License: BSD-2-Clause, Stephen Hurd. Preserve `LICENSE.md` and `LIBCRUX_NOTICE.txt` with binary packages. The source archive also contains public-domain SUPERCOP sntrup code; SFX does not register that KEX. ML-KEM includes Cryspen/Eurydice code under the preserved MIT terms.

The server registers only the two approved KEX algorithms, Ed25519, AES-256-CTR, HMAC-SHA2-256, and no compression. Having additional upstream algorithms in the exact source archive does not authorize registering them. In particular AES-128-CBC is never registered. A pin change or server cipher-policy change requires separate review.

The SFX build does not enable upstream example programs or test-only injection libraries. Integration evidence distinguishes actual OpenSSH/PuTTY Plink from the explicitly adapted Paramiko qualification client; no whole-upstream-test-suite pass is claimed.

## October 9 reviewed downstream receive-window overlay

The owner-authorized DR exact review clears only the `ssh-conn.c` overlay below.
It bounds replenished credit by physical stream-buffer space after subtracting
advertised, queued and in-flight credit, and rechecks after receive debit to avoid
a lost replenishment opportunity when the application reads during the callback.
No upstream pin, registered algorithm, crypto, buffer size or protocol limit changes.

- Source: `patches/receive-window-v2/ssh-conn.c`, SHA256 `2E6AC42217032431E6400C6B53842955CAA191D98603B9739B5D24E055A44DFB`.
- Full patch: `patches/receive-window-v2/receive-credit-and-liveness.patch`, SHA256 `A3149A4982F2B1BD3FF74AB94F7C973A55318F9C27F7B333F6B667A9C5CC5E31`.
- Incremental patch against the preserved credit-only revision: `patches/receive-window-v2/post-debit-liveness.patch`, SHA256 `E28DBA07F4922778AC9F993AA77EDA6C31A9EF50CE2F199C88C3C7F401CC51C5`.
- Original `ssh-conn.c`: SHA256 `3EEC29D82FF4542C97993E4F9C9BCE79FEC2F2181AAF2648AC0DE43CADED876A`.
- Earlier reviewed credit-only `patches/ssh-conn.c` and `patches/receive-credit.patch` remain unchanged; their SHA256 identities are `657D538CCD91CB4EB37FC6C9201A03B2E91D9FF021CC495D694ABDCB9B1207BB` and `49DB98FD14B54F9FABACA7EBEEB8A3A7FA1ACC0D38F7C36E3DB23B60EC86A91F`.

CMake checks the selected source/full-patch/original identities before creating
`reviewed-window-2e6ac4221703` in the build tree. The BSD license and other notices
remain exact. Isolated encrypted diagnostic results are not installed product or
physical owner acceptance. See `research/architecture/SECTION8B_SSH_LIVENESS_REVIEW.md`
for scope and evidence; the independent DR review itself did not execute tests.
