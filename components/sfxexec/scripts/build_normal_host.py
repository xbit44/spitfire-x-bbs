"""Build the normal SFX-owned host with explicit Runtime/pair identity pins.
No installed files are read or changed; no native execution is implied.
"""
from pathlib import Path
import hashlib,json,os,subprocess,sys,shutil
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
build=ROOT/'build'/('sfxexec-normal-host-'+sys.argv[1]);assert not build.exists();build.mkdir()
dll,dos,runtime=map(lambda s:Path(s).resolve(),sys.argv[2:5])
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest().upper()
assert sha(dll)=='D36589BDE11134B7F6019C32667B639BC465CA39CD39E22FEB6E053A2996A3F3','preserve qualified core'
pins=dict(SFX_DLL_SHA256=sha(dll),SFX_LAUNCHER_SHA256=sha(dos),SFX_RUNTIME_SHA256=sha(runtime))
(build/'pair_hashes.hpp').write_text('#pragma once\n'+''.join('#define '+k+' "'+v+'"\n' for k,v in pins.items()))
inputs=list((COMP/'host').glob('*.hpp'))+[COMP/'host/bridge.cpp',ROOT/'src/core/include/sfx/core/dos_host_protocol.hpp',Path(__file__)]
for source in inputs:
    copy=build/'inputs'/source.relative_to(ROOT);copy.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,copy)
envbat=build/'compiler-env.bat';envbat.write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat" >nul\nif errorlevel 1 exit /b 1\nset\n')
env={k.upper():v for k,v in os.environ.items()}
for line in subprocess.check_output(['cmd.exe','/d','/c',str(envbat)],text=True,encoding='mbcs').splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k.upper()]=v
compiler=Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86/cl.exe'
flags=['/nologo','/MT','/EHsc','/O2','/W4','/Gy','/std:c++17','/arch:IA32','/Brepro','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DSFX_NORMAL_HOST=1','/I'+str(COMP/'host'),'/I'+str(build),'/I'+str(ROOT/'src/core/include')]
args=[str(compiler),*flags,str(COMP/'host/bridge.cpp'),'/FeSFXDOSHost.exe','/link','/SUBSYSTEM:CONSOLE,6.01','/DYNAMICBASE','/NXCOMPAT','/Brepro','/OPT:REF','ws2_32.lib','advapi32.lib','iphlpapi.lib','bcrypt.lib']
result=subprocess.run(args,cwd=build,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90);(build/'build.log').write_bytes(result.stdout)
if result.returncode:raise SystemExit(result.stdout.decode('mbcs',errors='replace')[-6000:])
report=dict(result='PASS BUILD ONLY',commands=args,pins=pins,image_sha256=sha(build/'SFXDOSHost.exe'),inputs=[dict(path=str(p.relative_to(ROOT)),sha256=sha(p)) for p in inputs])
(build/'build.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS normal host compilation only; '+str(build/'SFXDOSHost.exe'))
