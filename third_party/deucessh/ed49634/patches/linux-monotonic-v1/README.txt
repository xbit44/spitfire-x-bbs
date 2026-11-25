SFX Linux monotonic clock boundary, November 8, 2026

Input is the existing SFX bounded-stream-v3/ssh-conn.c, SHA256
27472A62D961714E33DD6431E3BAAD927EB2EDE89C6B41F92DC6B5DFB4EE87F5.
The Linux-only variant replaces windows.h/GetTickCount64/ULONGLONG with
clock_gettime(CLOCK_MONOTONIC) and uint64_t. The 5000 ms wait, stream-credit
bounds and all protocol behavior remain unchanged. A clock failure causes
the bounded wait to expire. linux-monotonic.patch records the exact delta.
Other reviewed v3 overlay files and the original upstream archive stay exact.
Windows continues to build its existing v3 overlay. Retain upstream notices.
No ANet input or source was used.
