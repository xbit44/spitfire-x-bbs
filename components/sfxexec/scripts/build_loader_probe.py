"""Private diagnostic VDD, no entry point/CRT/static ntvdm import dependency."""
from pathlib import Path
import subprocess,os,sys,json,hashlib
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';BUILD=ROOT/'build'/('sfxexec-loader-probe-'+sys.argv[1]);assert not BUILD.exists();BUILD.mkdir()
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
bat=BUILD/'env.bat';bat.write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat" >nul\nset\n')
env={k.upper():v for k,v in os.environ.items()}
for line in subprocess.check_output(['cmd.exe','/d','/c',str(bat)],text=True,encoding='mbcs').splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k.upper()]=v
BIN=Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86';commands=[]
def run(args,name):
    args=[str(x) for x in args];commands.append(args);r=subprocess.run(args,cwd=BUILD,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60);(BUILD/name).write_bytes(r.stdout);assert r.returncode==0,r.stdout.decode(errors='replace')[-3000:]
run([BIN/'cl.exe','/nologo','/c','/W4','/O1','/Oi','/GS-','/Gz','/arch:IA32','/Brepro','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/I'+str(COMP/'src/src/sbbs3'),COMP/'src/load_probe.c'],'compile.log')
run([BIN/'link.exe','/NOLOGO','/DLL','/NOENTRY','/NODEFAULTLIB','/MACHINE:X86','/SUBSYSTEM:WINDOWS,6.01','/DYNAMICBASE','/NXCOMPAT','/Brepro','/OUT:SFXLDR.DLL','load_probe.obj','kernel32.lib'],'link.log')
run([BIN/'dumpbin.exe','/HEADERS','/IMPORTS','/EXPORTS',BUILD/'SFXLDR.DLL'],'pe.txt')
dump=(BUILD/'pe.txt').read_text();assert 'ntvdm.exe' not in dump.lower() and 'ucrt' not in dump.lower() and 'VCRUNTIME' not in dump and 'VDDDispatch' in dump
(BUILD/'build.json').write_text(json.dumps(dict(result='PASS build/import inspection only; native VDM loading PENDING',commands=commands,
    source=[dict(path=str(p.relative_to(COMP)),sha256=sha(p)) for p in [COMP/'src/load_probe.c',COMP/'src/src/sbbs3/sfx_load.h']],
    output=dict(path=str(BUILD/'SFXLDR.DLL'),bytes=(BUILD/'SFXLDR.DLL').stat().st_size,sha256=sha(BUILD/'SFXLDR.DLL'))),indent=2)+'\n')
print('PASS private no-CRT diagnostic SFXLDR.DLL',sha(BUILD/'SFXLDR.DLL'))
