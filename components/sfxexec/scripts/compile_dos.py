"""Run original Microsoft tools in private DOS build host, not a game runtime."""
from pathlib import Path
import os,subprocess,json,shutil,hashlib,sys,re
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
EV=ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024'
TOOLS=ROOT/'build/sfxexec-tools-20261024'
BUILD=ROOT/'build'/('sfxexec-dos-compile-'+sys.argv[1]);assert not BUILD.exists();BUILD.mkdir()
DRIVE=BUILD/'drive';DRIVE.mkdir()
for name in ('BIN','INCLUDE','LIB'):shutil.copytree(TOOLS/'MSVC15'/name,DRIVE/'VC'/name)
for name in ('INC','TMP','SRC/DOSXTRN'):(DRIVE/name).mkdir(parents=True,exist_ok=True)
shutil.copy2(TOOLS/'DDK/isvbop.h',DRIVE/'INC/isvbop.h')
headers=list((COMP/'src/src/xpdev').glob('*.h'))+[COMP/'src/src/sbbs3'/name for name in ('vdd_func.h','sfx_pair.h','sfx_load.h','sfx_gate.h','fossdefs.h','git_branch.h','git_hash.h')]
aliases={p.name:p.stem[:8]+p.suffix for p in headers}
assert len({v.lower() for v in aliases.values()})==len(aliases),'short-header collision'
def short_includes(text):
    return re.sub(r'(#\s*include\s*["<])([^">]+)([">])',lambda m:m[1]+str(Path(m[2]).with_name(aliases.get(Path(m[2]).name,Path(m[2]).name))).replace('\\','/')+m[3],text)
for header in headers:
    target=DRIVE/('INC' if header.parent.name=='xpdev' else 'SRC')/aliases[header.name]
    target.write_text(short_includes(header.read_text()))
(BUILD/'short-header-map.json').write_text(json.dumps(aliases,indent=2)+'\n')
if '--tiny' in sys.argv:(DRIVE/'SRC/DOSXTRN/SFXDOOR.C').write_text('int main(void) { return 0; }\n')
else:(DRIVE/'SRC/DOSXTRN/SFXDOOR.C').write_text(short_includes((COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text()))
(DRIVE/'SRC/DOSXTRN/SFXDOOR.DEF').write_text('EXETYPE DOS\n')
(DRIVE/'BUILD.BAT').write_text('@echo off\nset PATH=C:\\VC\\BIN\nset INCLUDE=C:\\VC\\INCLUDE;C:\\INC;C:\\SRC\nset LIB=C:\\VC\\LIB\nset TEMP=C:\\TMP\nset TMP=C:\\TMP\nC:\ncd \\SRC\\DOSXTRN\ncl /AS /Gs /Fc /Fm /FeSFXDOOR.EXE SFXDOOR.C > C:\\COMPILE.LOG\nif errorlevel 1 goto fail\necho PASS > C:\\PASS.TXT\ngoto done\n:fail\necho FAIL > C:\\FAIL.TXT\n:done\nexit\n')
conf=BUILD/'compiler.conf'
conf.write_text('[sdl]\nfullscreen=false\noutput=surface\nvideodriver=dummy\nautolock=false\n[sdl2]\n[mixer]\nnosound=true\n[dosbox]\nmemsize=32\n[render]\nframeskip=10\n[cpu]\ncore=normal\ncputype=486_slow\ncycles=fixed 50000\n[ne2000]\nne2000=false\n[ipx]\nipx=false\n[autoexec]\n@echo off\nmount c "'+str(DRIVE)+'"\nc:\\BUILD.BAT\nexit\n')
host=json.loads((EV/'compiler-host.json').read_text());env=os.environ.copy();env['SDL_VIDEODRIVER']='dummy';env['SDL_AUDIODRIVER']='dummy'
args=[host['exe'],'-conf',str(conf),'-fastlaunch','-exit','-nogui']
result=subprocess.run(args,cwd=Path(host['exe']).parent,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
    timeout=90,creationflags=subprocess.CREATE_NO_WINDOW)
(BUILD/'host.log').write_bytes(result.stdout)
log=DRIVE/'COMPILE.LOG'
if log.exists():print(log.read_text(errors='replace'),flush=True)
print('Host exit',result.returncode,'PASS marker',(DRIVE/'PASS.TXT').exists(),flush=True)
assert (DRIVE/'PASS.TXT').exists() and (DRIVE/'SRC/DOSXTRN/SFXDOOR.EXE').exists(),'compiler did not complete; retain all output'
exe=DRIVE/'SRC/DOSXTRN/SFXDOOR.EXE'
(BUILD/'build.json').write_text(json.dumps(dict(result='PASS Microsoft 1.52c compiler/linker under private build-only DOS host',
    launcher=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest().upper(),bytes=exe.stat().st_size,
    compiler_host=host,commands=args,tiny_probe='--tiny' in sys.argv,native_VDD_test=False),indent=2)+'\n')
