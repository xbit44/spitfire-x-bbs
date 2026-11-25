"""Actual candidate DLL fixture via private ntvdm import shim; no DOS claim."""
from pathlib import Path
import os,subprocess,sys,json,hashlib
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
BUILD=ROOT/'build'/('sfxexec-vdd-fixture-'+sys.argv[1]);assert not BUILD.exists();BUILD.mkdir()
DLL=Path(sys.argv[2]).resolve()
envbat=BUILD/'compiler-env.bat';envbat.write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat" >nul\nif errorlevel 1 exit /b 1\nset\n')
env={k.upper():v for k,v in os.environ.items()}
for line in subprocess.check_output(['cmd.exe','/d','/c',str(envbat)],text=True,encoding='mbcs').splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k.upper()]=v
bin=Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86'
def run(args,log):
    p=subprocess.run([str(a) for a in args],cwd=BUILD,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
    (BUILD/log).write_bytes(p.stdout)
    assert p.returncode==0,p.stdout.decode('mbcs',errors='replace')[-5000:]
exports='getAX getBH getBL getCX getES getDI setAX MGetVdmPointer call_ica_hw_interrupt VDDInstallIOHook VDDDeInstallIOHook TestRegisters TestBuffer TestResult'.split()
deffile=BUILD/'ntvdm.def';deffile.write_text('LIBRARY NTVDM.EXE\nEXPORTS\n '+'\n '.join(exports)+'\n')
run([bin/'cl.exe','/nologo','/MT','/LD','/Gz','/Di386','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601',
    COMP/'tests/ntvdm_stub.c','/link','/DEF:'+str(deffile),'/OUT:ntvdm.exe','/IMPLIB:ntvdm_fixture.lib'],'stub-build.log')
run([bin/'cl.exe','/nologo','/MT','/EHsc','/std:c++17','/O2','/D_WIN32_WINNT=0x0601',COMP/'tests/vdd_harness.cpp',
    'ntvdm_fixture.lib','advapi32.lib','/FeVDDHarness.exe'],'harness-build.log')
children=[]
for token in ('a'*32,'b'*32):
    output=(BUILD/(token[0]+'.log')).open('wb')
    child=subprocess.Popen([str(BUILD/'VDDHarness.exe'),str(DLL),token],cwd=BUILD,env=env,stdout=output,stderr=subprocess.STDOUT)
    children.append((child,output,token))
results=[]
for child,output,token in children:
    try:code=child.wait(timeout=30)
    except subprocess.TimeoutExpired:child.kill();child.wait();raise
    output.close();text=(BUILD/(token[0]+'.log')).read_text(errors='replace');print(text,flush=True)
    results.append(dict(case=token[0],exit=code,log=text))
assert all(r['exit']==0 for r in results),'candidate fixture failed; all failures retained'
(BUILD/'result.json').write_text(json.dumps(dict(result='PASS',candidate_sha256=hashlib.sha256(DLL.read_bytes()).hexdigest().upper(),
    tests=results,method='Two concurrent Win32 processes execute the actual candidate dispatcher and IPC. Both use visible node123, distinct sessions and bytes. VDM CPU/interrupt functions mocked; native DOS/Synchronet coexistence remains pending.'),indent=2)+'\n')
