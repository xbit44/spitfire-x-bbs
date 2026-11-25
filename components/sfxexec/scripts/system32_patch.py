"""Reproduce finite System32 launcher changes from retained exact r4.
No system file operations. Run after loader_patch.py when rebuilding source.
"""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
BASE=COMP/'baseline-r4'
if not BASE.exists():BASE=ROOT/'research/architecture/evidence/section11-sfxexec-system32-20261024/baseline-r4'
p=COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c';s=(BASE/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text()
s=s.replace('#include "sfx_load.h"','#include "sfx_gate.h"\n#include <time.h>')
a=s.index('/* Diagnose only after target registration failed.');b=s.index('int main(int argc',a)
s=s[:a]+'''/* Wait before installing DOS handlers/spawning a game. The host releases this
 * gate only after verifying the pinned System32 file and actual VDM module.
 * VDD_PEEK/READ/WRITE use unchanged bounded private session channels. */
static int sfx_wait_start(const char* session) {
    char expected[SFX_GATE_BOUND],received[SFX_GATE_BOUND],ack[SFX_GATE_BOUND];
    WORD segment;int available,got;time_t begin=time(NULL),now;
    sprintf(expected,SFX_GATE_PREFIX "%s|%u",session,node_num);
    _asm mov segment,ss;
    printf("SFXLOAD core open; awaiting host System32 identity approval\\n");fflush(stdout);
    do {
        if(vdd_op(VDD_ONLINE)!=1) return 0;
        available=vdd_op(VDD_INBUF_FULL);
        if(available<0 || available>SFX_GATE_BOUND-1) return 0;
        if(available>=(int)strlen(expected)) {
            got=vdd_buf(VDD_READ,available,segment,(WORD)received);
            if(!sfx_gate_matches(received,(unsigned)got,expected)) return 0;
            sprintf(ack,SFX_GATE_ACK "%s|%u",session,node_num);
            if(vdd_buf(VDD_WRITE,strlen(ack),segment,(WORD)ack)!=(int)strlen(ack)) return 0;
            printf("SFXLOAD host identity approval accepted; game may start\\n");fflush(stdout);
            return 1;
        }
        vdd_op(VDD_YIELD);now=time(NULL);
    } while(now>=begin && now-begin<10);
    return 0;
}

'''+s[b:]
s=s.replace('\tchar\tdll[256];\n','')
s=s.replace('\tsprintf(dll,"%s%s",exec_dir,VDD_FILENAME);\n\tDllName=dll;', '''    /* Native System32 convention: one exact basename, no alternate attempts.
     * Host pins the resolved System32 target and verifies actual load identity
     * before sending SFXSTART2. Registration alone never permits a game. */
    DllName=VDD_FILENAME;''')
s=s.replace('SFXLOAD direct path=%s','SFXLOAD System32-contract registration name=%s')
s=s.replace('/* Strict full-path load only. Never fall back to any other DLL. */','/* Exactly one approved basename registration; no fallback driver/probe. */')
s=s.replace('SFXLOAD direct RegisterModule FAILED','SFXLOAD System32 RegisterModule FAILED')
s=s.replace('sfx_load_diagnose(dll);return 18;','return 18;')
at=s.index('\toldint16=_dos_getvect(0x16);')
s=s[:at]+'''    if(!sfx_wait_start(argv[6])) {
        printf("SFXLOAD FAILED stage=host_identity_gate; game not started\\n");fflush(stdout);
        vdd_op(VDD_CLOSE);
        _asm mov ax,vdd;
        UnRegisterModule();return 19;
    }
'''+s[at:]
assert s.count('\n\t\tRegisterModule();')==1 and 'sfx_load_diagnose' not in s
p.write_text(s)
print('Applied exact-basename registration and pre-game host approval gate to r4 launcher.')
