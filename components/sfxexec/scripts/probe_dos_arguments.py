"""Exercise the exact launcher argument splitter and MSVC DOS spawn command tail.
This uses only the existing private compiler host; no VDD/game test is implied.
"""
from pathlib import Path
import os,sys,json,subprocess,shutil,hashlib
root=Path(__file__).resolve().parents[3]
build=root/'build'/('sfxexec-dos-arguments-'+sys.argv[1]);assert not build.exists();build.mkdir()
drive=build/'drive';drive.mkdir()
tools=root/'build/sfxexec-tools-20261024/MSVC15'
for name in ('BIN','INCLUDE','LIB'):shutil.copytree(tools/name,drive/'VC'/name)
(drive/'TMP').mkdir()
source=(root/'components/sfxexec/src/src/sbbs3/dosxtrn/dosxtrn.c').read_text()
splitter=source[source.index('static int sfx_arguments('):source.index('int main(int argc, char **argv)')]
(drive/'ARGTEST.C').write_text('#include <stdio.h>\n#include <string.h>\n#include <process.h>\n'+splitter+r'''
int main(void) {
 char one[128]="\"C:\\ECHOARG.EXE\" /d\"C:\\NODE DIR\\1\" \"hello world\" \"\" plain";
 char bad[128]="ECHOARG.EXE \"unterminated";
 char overflow[128]="ECHOARG.EXE 1 2 3 4";
 char* args[32]; FILE* fp; int result;
 if(sfx_arguments(bad,args,32)!=0 || sfx_arguments(overflow,args,3)!=0)return 2;
 if(sfx_arguments(one,args,32)!=5)return 3;
 result=_spawnvp(_P_WAIT,args[0],args);
 fp=fopen("RESULT.TXT","w");if(!fp)return 4;fprintf(fp,"%d\n",result);fclose(fp);
 return result==23?0:5;
}
''')
(drive/'ECHOARG.C').write_text(r'''
#include <stdio.h>
int main(int argc,char** argv) {
 FILE* fp=fopen("ARGV.TXT","w");int i;if(!fp)return 10;
 fprintf(fp,"%d\n",argc);for(i=1;i<argc;i++)fprintf(fp,"[%s]\n",argv[i]);fclose(fp);return 23;
}
''')
(drive/'BUILD.BAT').write_text('@echo off\nset PATH=C:\\VC\\BIN\nset INCLUDE=C:\\VC\\INCLUDE\nset LIB=C:\\VC\\LIB\nset TEMP=C:\\TMP\nset TMP=C:\\TMP\nC:\ncd \\\ncl /AS /Gs ECHOARG.C > COMPILE1.LOG\nif errorlevel 1 goto done\ncl /AS /Gs ARGTEST.C > COMPILE2.LOG\nif errorlevel 1 goto done\nARGTEST.EXE\nif errorlevel 1 goto done\necho PASS > PASS.TXT\n:done\nexit\n')
previous=root/'build/sfxexec-dos-compile-normal-r1/compiler.conf'
config=previous.read_text();start=config.index('mount c "');end=config.index('"',start+9)
config=config[:start]+'mount c "'+str(drive)+config[end:]
conf=build/'compiler.conf';conf.write_text(config)
host=json.loads((root/'research/architecture/evidence/section11-sfxexec-components-20261024/compiler-host.json').read_text())
env=os.environ.copy();env['SDL_VIDEODRIVER']='dummy';env['SDL_AUDIODRIVER']='dummy'
result=subprocess.run([host['exe'],'-conf',str(conf),'-fastlaunch','-exit','-nogui'],cwd=Path(host['exe']).parent,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90,creationflags=subprocess.CREATE_NO_WINDOW)
(build/'host.log').write_bytes(result.stdout)
assert (drive/'PASS.TXT').exists(),'DOS argument probe failed; retain logs'
argv=(drive/'ARGV.TXT').read_text();assert argv=='5\n[/dC:\\NODE DIR\\1]\n[hello world]\n[]\n[plain]\n',repr(argv)
assert (drive/'RESULT.TXT').read_text()=='23\n'
(build/'result.json').write_text(json.dumps(dict(result='PASS exact splitter and actual 16-bit MSVC spawn/child argv/exit',launcher_source_sha256=hashlib.sha256(source.encode()).hexdigest(),argv=argv,exit=23,native_VDD_test=False),indent=2)+'\n')
print('PASS DOS argv quoting, empty argument, numeric return, malformed/overflow refusal')
