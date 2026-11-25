"""Build independent minimum bridge after the paired binaries are frozen."""
from pathlib import Path
import sys,os,subprocess,json,hashlib
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
BUILD=ROOT/'build'/('sfxexec-host-'+sys.argv[1]);assert not BUILD.exists();BUILD.mkdir()
DLL=Path(sys.argv[2]).resolve();DOS=Path(sys.argv[3]).resolve()
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
(BUILD/'pair_hashes.hpp').write_text('#pragma once\n'+''.join('#define '+key+' "'+value+'"\n' for key,value in {
    'SFX_DLL_SHA256':sha(DLL),'SFX_LAUNCHER_SHA256':sha(DOS),'SFX_RUNTIME_SHA256':'90E2F01DE0BBD018830E30EC58CD3703710A769A286DB1FB39C06A5780AE0C8F'}.items()))
envbat=BUILD/'compiler-env.bat';envbat.write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat" >nul\nif errorlevel 1 exit /b 1\nset\n')
env={k.upper():v for k,v in os.environ.items()}
for line in subprocess.check_output(['cmd.exe','/d','/c',str(envbat)],text=True,encoding='mbcs').splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k.upper()]=v
BIN=Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86';commands=[]
def run(args,log,expected=0):
    args=[str(x) for x in args];commands.append(args);r=subprocess.run(args,cwd=BUILD,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
    (BUILD/log).write_bytes(r.stdout);assert r.returncode==expected,r.stdout.decode('mbcs',errors='replace')[-5000:];return r.stdout.decode('mbcs',errors='replace')
FLAGS=['/nologo','/MT','/EHsc','/O2','/W4','/std:c++17','/arch:IA32','/Brepro','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/I'+str(COMP/'host'),'/I'+str(BUILD)]
run([BIN/'cl.exe',*FLAGS,COMP/'host/bridge.cpp','/FeSFXDOSHost.exe','/link','/SUBSYSTEM:CONSOLE,6.01','/DYNAMICBASE','/NXCOMPAT','/Brepro','ws2_32.lib','advapi32.lib','iphlpapi.lib','bcrypt.lib'],'host-build.log')
run([BIN/'cl.exe',*FLAGS,COMP/'tests/bridge_model_tests.cpp','/FeBridgeModelTests.exe','/link','/SUBSYSTEM:CONSOLE,6.01'],'model-build.log')
print(run([BUILD/'BridgeModelTests.exe'],'model-tests.log'))
# This host is x64; refusal must precede any missing profile / installed path access.
print(run([BUILD/'SFXDOSHost.exe','--profile','deliberately-absent.ini'],'x64-refusal.log',1))
run([BIN/'dumpbin.exe','/HEADERS','/IMPORTS',BUILD/'SFXDOSHost.exe'],'host-pe.txt')
(BUILD/'build.json').write_text(json.dumps(dict(result='PASS build/model/x64 refusal; native x86 bridge PENDING',commands=commands,
    pair=dict(dll=sha(DLL),launcher=sha(DOS)),source=[dict(path=str(p.relative_to(COMP)),sha256=sha(p)) for p in sorted((COMP/'host').glob('*'))],
    generated_header_sha256=sha(BUILD/'pair_hashes.hpp'),host=dict(bytes=(BUILD/'SFXDOSHost.exe').stat().st_size,sha256=sha(BUILD/'SFXDOSHost.exe'))),indent=2)+'\n')
