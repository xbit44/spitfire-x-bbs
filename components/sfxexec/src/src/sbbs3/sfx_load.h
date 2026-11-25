/* SFX private loader diagnostic wire format, 2026-10-24.
 * GPL-2.0-or-later. Not part of the core VDD/session ABI. */
#ifndef SFX_LOAD_H
#define SFX_LOAD_H
#define SFX_LOAD_PAIR 0x64d1
#define SFX_LOAD_PATH 128
#define SFX_LOAD_REPORT 1024
typedef struct sfx_load_request {
    char path[SFX_LOAD_PATH];
    char report[SFX_LOAD_REPORT];
} sfx_load_request;
typedef char sfx_load_size[(sizeof(sfx_load_request)==1152)?1:-1];
#endif
