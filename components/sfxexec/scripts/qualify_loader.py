"""Synthetic actual-Windows loader cases; fake NTVDM, no native DOS claim."""
from pathlib import Path
import subprocess,sys,os,json,hashlib,shutil
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';BUILD=ROOT/'build'/('sfxexec-loader-fixture-'+sys.argv[1]);assert not BUILD.exists();BUILD.mkdir()
CORE=Path(sys.argv[2]).resolve();PROBE=Path(sys.argv[3]).resolve()
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
bat=BUILD/'env.bat';bat.write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat" >nul\nset\n')
env={k.upper():v for k,v in os.environ.items()}
for line in subprocess.check_output(['cmd.exe','/d','/c',str(bat)],text=True,encoding='mbcs').splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k.upper()]=v
BIN=Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86';X64=BIN.parent/'x64';commands=[];results=[]
def run(args,cwd,log):
    args=[str(x) for x in args];commands.append(args);r=subprocess.run(args,cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=40);(cwd/log).write_bytes(r.stdout);assert r.returncode==0,r.stdout.decode('mbcs',errors='replace')[-3000:];return r.stdout.decode('mbcs',errors='replace')
exports='getAX getBH getBL getCX getES getDI setAX MGetVdmPointer call_ica_hw_interrupt VDDInstallIOHook VDDDeInstallIOHook TestRegisters TestBuffer TestResult'.split()
for kind in ('good','missing','bits','api','dependency'):
    work=BUILD/kind;work.mkdir();shutil.copy2(PROBE,work/'SFXLDR.DLL');stub=(COMP/'tests/ntvdm_stub.c').read_text()
    if kind=='api':stub=stub.replace('__declspec(dllexport) BOOL VDDInstallIOHook(HANDLE handle,WORD count,void* range,void* handlers)\n{(void)handle;(void)count;(void)range;(void)handlers;return TRUE;}','')
    (work/'stub.c').write_text(stub);(work/'ntvdm.def').write_text('LIBRARY NTVDM.EXE\nEXPORTS\n '+'\n '.join(x for x in exports if kind!='api' or x!='VDDInstallIOHook')+'\n')
    run([BIN/'cl.exe','/nologo','/MT','/LD','/Gz','stub.c','/link','/DEF:ntvdm.def','/OUT:ntvdm.exe','/IMPLIB:ntvdm_fixture.lib'],work,'stub.log')
    run([BIN/'cl.exe','/nologo','/MT','/EHsc','/std:c++17','/O2','/I'+str(COMP/'src/src/sbbs3'),COMP/'tests/load_probe_harness.cpp','ntvdm_fixture.lib','/FeProbeHarness.exe'],work,'build.log')
    if kind in ('good','api'):shutil.copy2(CORE,work/'SFXEXEC.DLL')
    if kind=='bits':
        (work/'bits.c').write_text('__declspec(dllexport) void VDDDispatch(void) {}\n')
        run([X64/'cl.exe','/nologo','/c','/GS-','bits.c'],work,'bits-compile.log')
        run([X64/'link.exe','/NOLOGO','/DLL','/NOENTRY','/NODEFAULTLIB','/MACHINE:X64','/OUT:SFXEXEC.DLL','bits.obj'],work,'bits-link.log')
    if kind=='dependency':
        (work/'missing.def').write_text('LIBRARY SFXDIA_MISSING.dll\nEXPORTS\n MissingCall\n')
        run([BIN/'lib.exe','/NOLOGO','/DEF:missing.def','/MACHINE:X86','/OUT:missing.lib'],work,'missing-import.log')
        (work/'dependency.c').write_text('__declspec(dllimport) void MissingCall(void); __declspec(dllexport) void VDDDispatch(void) {MissingCall();}\n')
        run([BIN/'cl.exe','/nologo','/c','/GS-','dependency.c'],work,'dep-compile.log')
        run([BIN/'link.exe','/NOLOGO','/DLL','/NOENTRY','/NODEFAULTLIB','/MACHINE:X86','/OUT:SFXEXEC.DLL','dependency.obj','missing.lib'],work,'dep-link.log')
    out=run([work/'ProbeHarness.exe',work/'SFXLDR.DLL',work/'SFXEXEC.DLL',kind],work,'result.log');print(kind,out,flush=True);results.append(dict(kind=kind,output=out))
run([BIN/'cl.exe','/nologo','/MT','/EHsc','/std:c++17','/O2','/I'+str(COMP/'host'),COMP/'tests/pe_exports_tests.cpp','/FePEExportsTests.exe'],BUILD,'pe-build.log')
out=run([BUILD/'PEExportsTests.exe',BUILD/'good/ntvdm.exe'],BUILD,'pe-tests.log');print(out,flush=True)
(BUILD/'result.json').write_text(json.dumps(dict(result='PASS ordinary-process/mock VDM loader fixtures; native failure cause PENDING',core_sha256=sha(CORE),probe_sha256=sha(PROBE),commands=commands,tests=results,pe_test=out),indent=2)+'\n')
