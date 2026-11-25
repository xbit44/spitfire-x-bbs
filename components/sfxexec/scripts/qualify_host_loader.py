"""Actual host helper functions, fresh files and owned loopback only."""
from pathlib import Path
import os,subprocess,sys,json,shutil,hashlib
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';BUILD=ROOT/'build'/('sfxexec-host-loader-fixture-'+sys.argv[1]);assert not BUILD.exists();BUILD.mkdir()
HOST=Path(sys.argv[2]).resolve();CORE=Path(sys.argv[3]).resolve();DOS=Path(sys.argv[4]).resolve();PROBE=Path(sys.argv[5]).resolve()
env={k.upper():v for k,v in os.environ.items()}
for line in subprocess.check_output(['cmd.exe','/d','/c',str(HOST/'compiler-env.bat')],text=True,encoding='mbcs').splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k.upper()]=v
BIN=Path(env['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86';commands=[]
def run(args,log):
    args=[str(x) for x in args];commands.append(args);r=subprocess.run(args,cwd=BUILD,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=45);(BUILD/log).write_bytes(r.stdout);assert r.returncode==0,r.stdout.decode(errors='replace')[-3000:];return r.stdout.decode(errors='replace')
run([BIN/'cl.exe','/nologo','/MT','/EHsc','/std:c++17','/O2','/D_WIN32_WINNT=0x0601','/I'+str(COMP/'host'),'/I'+str(HOST),COMP/'tests/host_loader_tests.cpp','/FeHostLoaderTests.exe','/link','ws2_32.lib','advapi32.lib','iphlpapi.lib','bcrypt.lib'],'build.log')
files=BUILD/'ActualCandidateBIN';files.mkdir()
for p in (CORE,DOS,PROBE):shutil.copy2(p,files/p.name)
out=run([BUILD/'HostLoaderTests.exe',BUILD/'PrivateFixtures',files],'tests.log');print(out)
(BUILD/'result.json').write_text(json.dumps(dict(result='PASS actual host helper/file/socket tests; native VDM remains PENDING',commands=commands,output=out,
    host_source_sha256=hashlib.sha256((COMP/'host/bridge.cpp').read_bytes()).hexdigest().upper()),indent=2)+'\n')
