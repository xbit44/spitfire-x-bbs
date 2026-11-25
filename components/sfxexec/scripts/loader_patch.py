"""Reproduce loader-only diagnostic changes from exact retained r2 source."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';BASE=COMP/'baseline-r2'
if not BASE.exists():BASE=ROOT/'research/architecture/evidence/section11-sfxexec-loader-fix-20261024/baseline-source'
p=COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c';s=(BASE/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text()
def edit(a,b):
    global s
    assert a in s,a[:80];s=s.replace(a,b)
edit('#include "sfx_pair.h"','#include "sfx_pair.h"\n#include "sfx_load.h"')
at=s.index('int main(int argc, char **argv)')
s=s[:at]+'''/* Diagnose only after target registration failed. Strict full-path probe,
 * dynamically resolved NTVDM services and no game execution on this branch. */
static sfx_load_request load_request;
static void sfx_load_diagnose(const char* target) {
    static char probe[256];char* slash;char far* request_pointer;BOOL loaded=FALSE;WORD result,segment;
    strcpy(probe,target);slash=getfname(probe);strcpy(slash,"SFXLDR.DLL");
    DllName=probe;printf("SFXLOAD diagnostic probe path=%s; no search fallback\\n",probe);fflush(stdout);
    _asm {
        push es
        mov si,DllName
        mov bx,DispFunc
        xor di,di
        mov es,di
    }
    RegisterModule();
    _asm {
        mov vdd,ax
        jc failed
        mov loaded,TRUE
    failed:
        pop es
    }
    if(!loaded){printf("SFXLOAD probe RegisterModule BOP AX=%u; Win32 load detail unavailable\\n",(unsigned)vdd);fflush(stdout);return;}
    if(vdd_op(21)!=SFX_LOAD_PAIR){printf("SFXLOAD probe identity/reflected VDM ABI unavailable\\n");}
    else {
        memset(&load_request,0,sizeof(load_request));strcpy(load_request.path,target);
        request_pointer=(char far*)&load_request;segment=FP_SEG(request_pointer);
        result=vdd_buf(0,sizeof(load_request),segment,FP_OFF(request_pointer));
        if(result==0){load_request.report[SFX_LOAD_REPORT-1]=0;printf("%s",load_request.report);}
        else printf("SFXLOAD native diagnostic unavailable: probe dispatch=0x%04x\\n",result);
    }
    _asm mov ax,vdd;
    UnRegisterModule();fflush(stdout);
}

'''+s[at:]
edit('WORD\tbuf_seg;\n\tWORD\tw;','WORD\tbuf_seg;\n\tWORD\tw;\n    WORD load_ds,load_ss;unsigned readback_ok=1,j;')
edit('\t\t/* Register VDD */','''        _asm mov load_ds,ds;
        _asm mov load_ss,ss;
        for(j=0;j<=strlen(DllName);++j)if(((char far*)_MK_FP(load_ds,(WORD)DllName))[j]!=DllName[j])readback_ok=0;
        printf("SFXLOAD direct path=%s node=%u DS=%04x SS=%04x SI=%04x readback=%u init=0000:0000\\n",DllName,node_num,load_ds,load_ss,(unsigned)DllName,readback_ok);fflush(stdout);
        if(!readback_ok){printf("SFXLOAD DS:SI readback mismatch; registration/game refused\\n");return 18;}
        /* Register VDD */''')
edit('fprintf(stderr,"Error %d loading %s\\n",vdd,DllName);\n\t\treturn(-1);',
'''printf("SFXLOAD direct RegisterModule FAILED BOP AX=%u path=%s; not a Win32 error\\n",(unsigned)vdd,DllName);fflush(stdout);
        sfx_load_diagnose(dll);return 18;''')
edit('\tvdd_str(VDD_LOAD_INI_SECTION, getfname(arg[0]));','\tprintf("SFXLOAD pair accepted; private INI accepted; opening core VDD\\n");fflush(stdout);\n\tvdd_str(VDD_LOAD_INI_SECTION, getfname(arg[0]));')
edit('fprintf(stderr,"!SFX DLL/launcher pair or session mismatch\\n");','printf("SFXLOAD FAILED stage=pair_or_bind; game not started\\n");fflush(stdout);')
edit('fprintf(stderr,"!VDD_OPEN ERROR: %d\\n",i);','printf("SFXLOAD FAILED stage=VDD_OPEN code=%d; game not started\\n",i);fflush(stdout);')
p.write_text(s)
print('PASS loader-only diagnostics derived from preserved r2 source; no fallback/fix claim')
