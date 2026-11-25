"""Reproduce the bounded SFX changes from preserved official source."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def replace(text,old,new):
    assert old in text,old[:100]
    return text.replace(old,new)
def main():
    driver=(ROOT/'upstream/src/sbbs3/sbbsexec.c').read_text()
    driver=replace(driver,'#include "vdd_func.h"','#include "vdd_func.h"\n#include "sfx_pair.h"')
    driver=driver.replace('"sbbsexec.ini"','"sfxexec.ini"').replace('"SBBS: %s','"SFXEXEC: %s')
    driver=driver.replace('sbbsexec%d.log','sfxexec%d.log').replace('Synchronet Virtual Device Driver','SFX Virtual Device Driver')
    driver=replace(driver,'str_list_t ini;','static char sfx_session[33];\nstatic BOOL sfx_bound;\nstr_list_t ini;')
    driver=replace(driver,'node_num = getBH();', '''node_num = getBH();
	if (node_num<1 || node_num>SFX_NODE_MAX) { setAX(0xfffe); return; }
	if (getBL()!=SFX_VDD_PAIR && getBL()!=SFX_VDD_BIND && !sfx_bound) { setAX(0xffff); return; }''')
    driver=replace(driver,'switch (getBL()) {','''switch (getBL()) {
		case SFX_VDD_PAIR: retval=SFX_PAIR_WORD; break;
		case SFX_VDD_BIND:
			if (sfx_bound || getCX()!=32) { retval=0xffff; break; }
			p=(BYTE*)GetVDMPointer((ULONG)((getES()<<16)|getDI()),32,FALSE);
			if (p==NULL) { retval=0xffff; break; }
			memcpy(sfx_session,p,32);sfx_session[32]=0;
			if (!sfx_valid_session(sfx_session)) { sfx_session[0]=0;retval=0xffff;break; }
			sfx_bound=TRUE;retval=SFX_PAIR_WORD;break;''')
    driver=replace(driver,'sprintf(str, "\\\\\\\\.\\\\mailslot\\\\sbbsexec\\\\wr%d", node_num);','sfx_name(str,sizeof(str),"to_dos",sfx_session,node_num);')
    driver=replace(driver,'sprintf(str, "\\\\\\\\.\\\\mailslot\\\\sbbsexec\\\\rd%d", node_num);','sfx_name(str,sizeof(str),"from_dos",sfx_session,node_num);')
    for name in ('hungup','hangup','carrier'):
        driver=replace(driver,f'sprintf(str, "sbbsexec_{name}%d", node_num);',f'sfx_name(str,sizeof(str),"{name}",sfx_session,node_num);')
    # No borrowed/default/cwd configuration search. Only the exact private file
    # supplied by our paired host/launcher is eligible.
    start=driver.index('\t\t\t/* Load cwd/sbbsexec.ini second')
    end=driver.index('\n\t\t}',start)
    driver=driver[:start]+driver[end:]
    driver=replace(driver,'if (isdir(p))','if (getCX()>MAX_PATH || p==NULL) { retval=0xffff; break; }\n\t\t\tif (isdir(p))')
    # The first output message reports the real module path for host-side
    # equality/hash verification before any game output can be accepted.
    driver=replace(driver,'lprintf(LOG_DEBUG, "VDD_OPEN: Opened successfully (wrslot=%p)", wrslot);','''{
				HMODULE self=NULL;char module[MAX_PATH+1];char hello[512];DWORD sent=0;
				if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCSTR)VDDDispatch,&self)||!GetModuleFileNameA(self,module,sizeof(module))) { retval=5;break; }
				_snprintf(hello,sizeof(hello),"SFXPAIR1|%s|%s|%u|%s",SFX_PAIR_ID,sfx_session,node_num,module);
				hello[sizeof(hello)-1]=0;
				if (!WriteFile(wrslot,hello,(DWORD)strlen(hello),&sent,NULL)||sent!=strlen(hello)) { retval=6;break; }
			}
			lprintf(LOG_DEBUG, "VDD_OPEN: Opened successfully (wrslot=%p)", wrslot);''')
    (ROOT/'src/src/sbbs3/sbbsexec.c').write_text(driver)
    launcher=(ROOT/'upstream/src/sbbs3/dosxtrn/dosxtrn.c').read_text()
    launcher=replace(launcher,'#include "vdd_func.h"','#include "vdd_func.h"\n#include "sfx_pair.h"')
    launcher=launcher.replace('"sbbsexec.dll"','"SFXEXEC.DLL"').replace('Synchronet FOSSIL Driver (DOSXTRN)','SFX FOSSIL Driver (SFXDOOR)')
    launcher=replace(launcher,'if(argc<2) {','if(argc!=7) {')
    launcher=launcher.replace('usage: dosxtrn <path/dosxtrn.env> [NT|x64] [node_num] [mode] [ini_file]',
        'usage: SFXDOOR <private.env> NT <node1-128> <mode> <private.ini> <session32hex>')
    launcher=replace(launcher,'sprintf(exec_dir,"%.*s",sizeof(exec_dir)-1,argv[0]);','''if(strcmp(argv[2],"NT") || !sfx_valid_session(argv[6])) return 10;
	{
		char* end;long node=strtol(argv[3],&end,10);
		if(!*argv[3] || *end || node<1 || node>SFX_NODE_MAX) return 11;
	}
	if(strlen(argv[0])>=sizeof(exec_dir)-16 || strlen(argv[1])<4) return 12;
	sprintf(exec_dir,"%.*s",sizeof(exec_dir)-1,argv[0]);''')
    launcher=replace(launcher,'for(i=0;i<2;i++) {','for(i=0;i<1;i++) {')
    launcher=replace(launcher,'DllName=VDD_FILENAME;\t/* try again with no path (for Windows Vista) */','/* Strict full-path load only. Never fall back to any other DLL. */')
    launcher=replace(launcher,'\tvdd_str(VDD_LOAD_INI_FILE, ini_fname);','''	if(vdd_op(SFX_VDD_PAIR)!=SFX_PAIR_WORD || vdd_str(SFX_VDD_BIND,argv[6])!=SFX_PAIR_WORD) {
		fprintf(stderr,"!SFX DLL/launcher pair or session mismatch\\n");
		_asm mov ax,vdd;
		UnRegisterModule();return 13;
	}
	vdd_str(VDD_LOAD_INI_FILE, ini_fname);''')
    launcher=replace(launcher,'\t\tUnRegisterModule();\n\t\treturn(-1);','\t\t_asm mov ax,vdd;\n\t\tUnRegisterModule();\n\t\treturn(-1);')
    (ROOT/'src/src/sbbs3/dosxtrn/dosxtrn.c').write_text(launcher)
    (ROOT/'src/src/sbbs3/git_branch.h').write_text('#define GIT_BRANCH "sfxexec-candidate"\n')
    (ROOT/'src/src/sbbs3/git_hash.h').write_text('#define GIT_HASH "97fab8d8+sfx-01"\n#define GIT_DATE "2026-10-24"\n')
    print('PASS explicit paired ABI, private session/node identities and strict SFX loader derivation')
if __name__=='__main__':main()
