/* SFX paired VDD additions, 2026. GPL-2.0-or-later, as the containing driver.
 * Explicit new dispatch operations; original operations 0..19 are unchanged. */
#ifndef SFX_PAIR_H
#define SFX_PAIR_H
#define SFX_PAIR_ID "SFXEXEC-20261024-01"
#define SFX_PAIR_WORD 0x6424
#define SFX_VDD_BIND 20
#define SFX_VDD_PAIR 21
#define SFX_SESSION_CHARS 32
#define SFX_NODE_MAX 128
static int sfx_valid_session(const char* text)
{
    unsigned i;
    if (text == 0) return 0;
    for (i=0;i<SFX_SESSION_CHARS;i++)
        if (!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f'))) return 0;
    return text[SFX_SESSION_CHARS]==0;
}
#ifdef _WIN32
static int sfx_name(char* out,unsigned size,const char* kind,const char* session,unsigned node)
{
    int count;
    if (!sfx_valid_session(session)||node<1||node>SFX_NODE_MAX||size<128) return 0;
    if (!strcmp(kind,"to_dos")||!strcmp(kind,"from_dos"))
        count=_snprintf(out,size,"\\\\.\\mailslot\\sfxexec\\v1\\%s\\n%u\\%s",session,node,kind);
    else count=_snprintf(out,size,"Local\\SFXEXEC.v1.%s.n%u.%s",session,node,kind);
    out[size-1]=0;return count>0&&(unsigned)count<size;
}
#endif
#endif
