"""Owned temporary ordinary-process module/IPC tests; never real System32/VDM."""
from pathlib import Path
import os,subprocess,sys,json,shutil,hashlib,re
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
BUILD=ROOT/'build'/('sfxexec-system32-fixture-'+sys.argv[1]);assert not BUILD.exists();BUILD.mkdir()
HOST=Path(sys.argv[2]).resolve();CORE=Path(sys.argv[3]).resolve();DOS=Path(sys.argv[4]).resolve()
env={k.upper():v for k,v in os.environ.items()}
for line in subprocess.check_output(['cmd.exe','/d','/c',str(HOST/'compiler-env.bat')],text=True,encoding='mbcs').splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k.upper()]=v
BIN=Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86';commands=[]
def run(args,log):
    args=[str(x) for x in args];commands.append(args);r=subprocess.run(args,cwd=BUILD,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=45)
    (BUILD/log).write_bytes(r.stdout);assert r.returncode==0,r.stdout.decode(errors='replace')[-4000:];return r.stdout.decode(errors='replace')
exports='getAX getBH getBL getCX getES getDI setAX MGetVdmPointer call_ica_hw_interrupt VDDInstallIOHook VDDDeInstallIOHook TestRegisters TestBuffer TestResult'.split()
(BUILD/'ntvdm.def').write_text('LIBRARY NTVDM.EXE\nEXPORTS\n '+'\n '.join(exports)+'\n')
run([BIN/'cl.exe','/nologo','/MT','/LD','/Gz','/Di386','/D_WIN32_WINNT=0x0601',COMP/'tests/ntvdm_stub.c','/link','/DEF:'+str(BUILD/'ntvdm.def'),'/OUT:ntvdm.exe','/IMPLIB:fixture.lib'],'shim-build.log')
flags=['/nologo','/MT','/EHsc','/std:c++17','/O2','/D_WIN32_WINNT=0x0601','/I'+str(COMP/'host'),'/I'+str(COMP/'src/src/sbbs3'),'/I'+str(HOST),'/I'+str(BUILD)]
launcher=(COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text();gate=launcher[launcher.index('static int sfx_wait_start('):launcher.index('\nint main(int argc')]
gate=gate.replace('_asm mov segment,ss;','segment=0; /* fixture maps pointer directly instead of SS:offset */').replace('(WORD)received','received').replace('(WORD)ack','ack')
(BUILD/'launcher_gate.inc').write_text(gate)
for src,exe in [('system32_host_tests.cpp','SystemHostTests.exe'),('system32_gate_tests.cpp','SystemGateTests.exe')]:
    run([BIN/'cl.exe',*flags,COMP/'tests'/src,'fixture.lib','/Fe'+exe,'/link','/SUBSYSTEM:CONSOLE,6.01','ws2_32.lib','advapi32.lib','iphlpapi.lib','bcrypt.lib'],exe+'.build.log')
files=BUILD/'CandidateFiles';files.mkdir()
for p in (CORE,DOS):shutil.copy2(p,files/p.name)
host=run([BUILD/'SystemHostTests.exe',BUILD/'OwnedTemporaryFiles',files],'host-tests.log');print(host,flush=True)
children=[]
for letter in ('c','d'):
    log=(BUILD/('gate-'+letter+'.log')).open('wb');p=subprocess.Popen([str(BUILD/'SystemGateTests.exe'),str(CORE),letter*32],cwd=BUILD,env=env,stdout=log,stderr=subprocess.STDOUT);children.append((p,log,letter))
gate_results=[]
for p,log,letter in children:
    try:code=p.wait(timeout=40)
    except subprocess.TimeoutExpired:p.kill();p.wait();raise
    log.close();out=(BUILD/('gate-'+letter+'.log')).read_text();print(out,flush=True);gate_results.append(dict(exit=code,output=out))
assert all(r['exit']==0 for r in gate_results),'retain failed gate evidence'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
count=lambda text:int(re.search(r'PASS (\d+)',text)[1])
(BUILD/'result.json').write_text(json.dumps(dict(result='PASS owned ordinary-process module/core IPC/launcher gate; native VDM PENDING',commands=commands,host_checks=count(host),gate_checks=sum(count(r['output']) for r in gate_results),host_output=host,gate_results=gate_results,
    core_sha256=sha(CORE),launcher_sha256=sha(DOS),source=[dict(path=p.relative_to(COMP).as_posix(),sha256=sha(p)) for p in [COMP/'host/bridge.cpp',COMP/'host/system_module.hpp',COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c',COMP/'src/src/sbbs3/sfx_gate.h',COMP/'tests/system32_host_tests.cpp',COMP/'tests/system32_gate_tests.cpp']],no_system32_access=True),indent=2)+'\n')
