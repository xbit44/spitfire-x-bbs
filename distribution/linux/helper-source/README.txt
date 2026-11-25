SFX LINUX TRANSFER HELPERS - CORRESPONDING SOURCE

This companion contains the complete source used for SFXFileTransfer and
SFXQWKTransfer in the matching Linux AMD64 test package. The helpers are
separate programs. They do not link to the SFX Runtime libraries.

Build on Linux with a C compiler, CMake 3.16 or newer, and Make or Ninja.
The engineering build uses GCC 9.4 on Ubuntu 20.04, targeting AMD64.
No network download is part of this build.

From this folder:

  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target SFXFileTransfer SFXQWKTransfer --parallel 2
  ./build/SFXFileTransfer --version
  ./build/SFXQWKTransfer --version

The two executable files are in build/. They are launched by SFX for an
already-authorized caller transfer, not as standalone public services.

Read third_party/sfx_file_transfer_linux/20261108/NOTICES.md, the licenses
folder there, and LINUX_CHANGES.txt for original credits, license terms and
the Linux changes. Source comments and original notices are retained.
MANIFEST.json binds every file in this companion to its SHA-256 checksum.

Build this companion in its own folder. Do not use sudo or replace files in
a running BBS. The main Linux package installer handles normal SFX updates.
