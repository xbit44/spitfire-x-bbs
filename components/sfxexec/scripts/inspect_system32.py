"""Exact current source/image binding, startup contract and preservation checks."""
from pathlib import Path
import json,hashlib,re,struct
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
EV=ROOT/'research/architecture/evidence/section11-sfxexec-system32-20261024'
CORE=ROOT/'build/sfxexec-components-r8/SFXEXEC.DLL';DOS=ROOT/'build/sfxexec-dos-compile-system32-r1';HOST=ROOT/'build/sfxexec-host-system32-r4';FIX=ROOT/'build/sfxexec-system32-fixture-r3'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
checks=[]
def ok(v,why):assert v,why;checks.append(why)
old=json.loads((EV/'baseline-r4.json').read_text());changed=[]
for r in old['files']:
    ok(sha(EV/'baseline-r4'/r['path'])==r['sha256'],'r4 preimage '+r['path'])
    if sha(COMP/r['path'])!=r['sha256']:changed.append(r['path'])
ok(set(changed)=={'host/bridge.cpp','scripts/build_host.py','scripts/compile_dos.py','src/src/sbbs3/dosxtrn/dosxtrn.c'},'four old component inputs changed')
for r in old['archives']:ok(sha(r['archive'])==r['sha256'] and Path(r['archive']).stat().st_size==r['bytes'],'r4 archive preserved')
prior=ROOT/'research/architecture/evidence/section11-sfxexec-loader-fix-20261024'
for r in json.loads((prior/'baseline-r2.json').read_text())['files']:ok(sha(prior/'baseline-source'/r['path'])==r['sha256'],'r2 source retained '+r['path'])
for r in json.loads((ROOT/'build/sfxexec-components-r8/build.json').read_text())['source']:
    if r['path']=='src/src/sbbs3/dosxtrn/dosxtrn.c':continue
    ok(sha(COMP/r['path'])==r['sha256'],'core source unchanged '+r['path'])
for r in json.loads((COMP/'UPSTREAM.json').read_text())['files']:ok(sha(COMP/'upstream'/r['path'])==r['sha256'],'pinned upstream '+r['path'])
for r in json.loads((HOST/'build.json').read_text())['source']:ok(sha(COMP/r['path'])==r['sha256'],'final host compiled source '+r['path'])
qualified=json.loads((FIX/'result.json').read_text())
for r in qualified['source']:ok(sha(COMP/r['path'])==r['sha256'],'final fixture qualified source '+r['path'])
ok(qualified['core_sha256']==sha(CORE),'fixture core binding');ok(qualified['launcher_sha256']==sha(DOS/'drive/SRC/DOSXTRN/SFXDOOR.EXE'),'fixture launcher binding')
aliases=json.loads((DOS/'short-header-map.json').read_text())
def short(s):return re.sub(r'(#\s*include\s*["<])([^">]+)([">])',lambda m:m[1]+str(Path(m[2]).with_name(aliases.get(Path(m[2]).name,Path(m[2]).name))).replace('\\','/')+m[3],s)
launcher=(COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text()
ok((DOS/'drive/SRC/DOSXTRN/SFXDOOR.C').read_text()==short(launcher),'exact DOS staged source')
for p in (COMP/'src/src').rglob('*.h'):
    if p.name not in aliases:continue
    q=DOS/'drive'/('INC' if p.parent.name=='xpdev' else 'SRC')/aliases[p.name];ok(q.read_text()==short(p.read_text()),'actual DOS header '+p.name)
ok('DllName=VDD_FILENAME;' in launcher and launcher.count('\n\t\tRegisterModule();')==1,'one exact-basename register convention')
ok('sfx_load_diagnose' not in launcher and 'SFXLDR.DLL' not in launcher,'no probe/alternate registration fallback')
ok(launcher.index('if(!sfx_wait_start(argv[6]))')<launcher.index('_spawnvp('),'host approval before game spawn')
ok('UnRegisterModule();return 19;' in launcher and 'return 18;' in launcher,'failed registration/gate refuses game')
ok('not a Win32 error' in launcher,'BOP AX distinct from Win32')
host=(COMP/'host/bridge.cpp').read_text();module=(COMP/'host/system_module.hpp').read_text()
ok(host.index('IsWow64Process(')<host.index('PairPins pins(base,system_driver())'),'x64 refused before native system selection')
ok('GetSystemDirectoryW' in module and 'GetFinalPathNameByHandleW' in module and 'CreateToolhelp32Snapshot' in module,'resolved system path, canonical file identity and actual module enumeration')
ok(host.index('startup_approval(hello,')<host.index('WriteFile(input.h,approval.data()')<host.index('startup_acknowledged(output.h')<host.index("send_all(client,std::string(1,'\\0'))"),'identity before approval before caller ACK')
ok('GetLastError()' in module and 'nNumberOfLinks==1' in module,'ordinary file and bounded module snapshot handling')
basehost=(EV/'baseline-r4/host/bridge.cpp').read_text()
start='    auto deadline=GetTickCount64()+drop.minutes*60000ull;'
end='int main(int argc,char** argv)'
ok(host[host.index(start):host.index(end)]==basehost[basehost.index(start):basehost.index(end)],'normal relay/credit/drain/exit code exact r4')
ok('CanDisconnect=false\\r\\n[UART]\\r\\nComPort=1' in host,'private INI/DTR policy preserved')
images=[]
for p in (CORE,DOS/'drive/SRC/DOSXTRN/SFXDOOR.EXE',HOST/'SFXDOSHost.exe'):
    data=p.read_bytes();ok(data[:2]==b'MZ','MZ '+p.name)
    if p.name=='SFXDOOR.EXE':
        w=struct.unpack_from('<14H',data);header=w[4]*16
        ok((w[2]-1)*512+(w[1] or 512)==len(data),'DOS exact MZ length')
        ok(w[12]+w[3]*4<=header and header+w[11]*16+w[10]<len(data),'DOS entry/relocation table')
        for n in range(w[3]):off,seg=struct.unpack_from('<HH',data,w[12]+n*4);ok(header+seg*16+off+2<=len(data),'DOS relocation range')
        ok(data.count(b'\xc4\xc4\x58\x00')==1,'one emitted RegisterModule BOP')
        ok(b'SFXEXEC.DLL' in data and b'SFXLDR.DLL' not in data,'exact driver name no probe')
    else:
        pe=struct.unpack_from('<I',data,60)[0];opt=pe+24
        ok(data[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',data,pe+4)[0]==0x14c,'PE32 x86 '+p.name)
        ok(struct.unpack_from('<H',data,opt)[0]==0x10b and struct.unpack_from('<HH',data,opt+48)==(6,1),'Win7 subsystem6.01 '+p.name)
        dump=(ROOT/'build/sfxexec-components-r8/SFXEXEC-pe.txt' if p.name=='SFXEXEC.DLL' else HOST/'host-pe.txt').read_text()
        ok(not re.search(r'(?i)\b(?:VCRUNTIME\d*\.dll|MSVCP\d*\.dll|ucrtbase\.dll|api-ms-win-crt)',dump),'static CRT '+p.name)
        ok(not any(s in dump for s in ('GetSystemTimePreciseAsFileTime','WaitOnAddress','GetTempPath2','GetCurrentPackageId')),'no identified post-Win7 direct imports '+p.name)
    images.append(dict(name=p.name,bytes=len(data),sha256=sha(p)))
ok(sha(CORE)=='D36589BDE11134B7F6019C32667B639BC465CA39CD39E22FEB6E053A2996A3F3','core exact r2/r4')
accepted=ROOT/'build/section11-doors-actions-product-r3'
for r in json.loads((accepted/'source-inputs-before.json').read_text()):ok(sha(accepted/'source-at-start'/r['path'])==r['sha256'],'accepted830 frozen input '+r['path'])
queued=json.loads((ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024/queued-category-source.json').read_text())
for r in queued['changes']:ok(sha(ROOT/r['path'])==r['after'],'queued R246 preserved '+r['path'])
ok(sha(ROOT/'test/integration/door_first_run_defaults.inc')==queued['new_include_sha256'],'queued include preserved')
for filename,h in [('SFX-1.0.0-alpha.2-SFXEXEC-Component-Test-20261024-r2.zip','230EF851A9B112045CD5A5E9B5065301D1200D669811CBDA51A9F5DD4BE089D7'),('SFX-1.0.0-alpha.2-SFXEXEC-Component-Source-20261024-r2.zip','1BA426F8A80FF33C0B7ACFAC33C5D1160B7A79E5F66E343421AD26FE3B6532A8'),('SFX-1.0.0-alpha.2-Section11-OwnerFixes-Windows-Owner-Test-20261024-r1.zip','CE56C08B3133AE8E0D31ED5C6000CA73DB4C2689E653F59ECD85CC3F420085D4'),('SFX-1.0.0-alpha.2-Section11-OwnerFixes-Engineering-Companion-20261024-r1.zip','FDFAEBCF86F6B04E8A3BCC6F93B5C9313D041F09F6F6904DEBFE217999BEBD4C')]:ok(sha(ROOT/'build'/filename)==h,'prior media preserved '+filename)
ok('PASS 28' in (EV/'placement-r1.log').read_text(encoding='utf-8-sig'),'exact document placement tests passed')
ok('PASS 56' in (EV/'rlogin-r2.log').read_text(encoding='utf-8-sig'),'accepted loopback regression passed')
files=[dict(path=p.relative_to(COMP).as_posix(),bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(COMP.rglob('*')) if p.is_file()]
binding=hashlib.sha256(json.dumps(files,sort_keys=True).encode()).hexdigest().upper()
out=dict(result='PASS local qualification/preservation; STOP for DR review before placement; native PENDING',checks=len(checks),images=images,files=files,binding_sha256=binding,changed_r4_files=changed,host_checks=qualified['host_checks'],gate_checks=qualified['gate_checks'],placement_checks=28,model_checks=80,rlogin_checks=56,inherited_core_checks=5470,core_unchanged=True)
(EV/'inspection-r1.json').write_text(json.dumps(out,indent=2)+'\n');print('PASS',len(checks),'static/source/image/preservation checks; binding',binding)
