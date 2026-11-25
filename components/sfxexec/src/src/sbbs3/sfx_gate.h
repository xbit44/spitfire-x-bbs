/* Launcher-only startup gate, GPL-2.0-or-later. Core operations are unchanged.
 * Host and launcher reserve one input/output message before any game/caller I/O.
 * 32-hex session and real node bind both directions; no reusable global signal.
 */
#ifndef SFX_GATE_H
#define SFX_GATE_H
#define SFX_GATE_PREFIX "SFXSTART2|"
#define SFX_GATE_ACK "SFXSTARTED2|"
#define SFX_GATE_BOUND 64
static int sfx_gate_matches(const char* bytes,unsigned count,const char* expected)
{
    unsigned size=(unsigned)strlen(expected);
    return count==size && size<SFX_GATE_BOUND && memcmp(bytes,expected,size)==0;
}
#endif
