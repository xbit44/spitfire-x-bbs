# SecLists 2026.1 strong-password blocklist provenance

- Upstream: <https://github.com/danielmiessler/SecLists>
- Release/tag: `2026.1`, published 2026-03-23
- Exact commit: `190c6f7bd58c847ceadfe57d9853592737f059e8`
- File: `xato-net-10-million-passwords-10000.txt`
- Upstream path: `Passwords/Common-Credentials/xato-net-10-million-passwords-10000.txt`
- Size/count: 76,497 bytes / 10,000 lines
- File SHA-256: `C63D5E4CCC31344D662583CC39CA4BD5BD20517FF1D24501F0C4E0C22D9B722A`
- License: MIT; Copyright (c) 2018 Daniel Miessler
- Exact `LICENSE.txt` SHA-256: `3DBDC93D5F8829DE0941744841730A09C106D0732E5AE0E98CA1D77BE7DED66C`
- Update policy: packaged local data only, changed through reviewed SFX releases; no runtime downloads

The complete proposed password is compared case-insensitively for blocklist
purposes only. Substring matching is prohibited. A missing or hash-mismatched
file fails Strong-mode creation/change closed without disabling verification of
existing credentials.
