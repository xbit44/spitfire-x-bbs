"""Exact r2 preservation and final loader-only source/ABI/image inspection."""
from pathlib import Path
import json,hashlib,re,struct
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';EV=ROOT/'research/architecture/evidence/section11-sfxexec-loader-fix-20261024';BASE=EV/'baseline-source'
CORE=ROOT/'build/sfxexec-components-r8/SFXEXEC.DLL';DOS=ROOT/'build/sfxexec-dos-compile-loader-r2';HOST=ROOT/'build/sfxexec-host-loader-r3';PROBE=ROOT/'build/sfxexec-loader-probe-r1'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
checks=[]
def ok(v,why):assert v,why;checks.append(why)
baseline=json.loads((EV/'baseline-r2.json').read_text());changed=[]
for r in baseline['files']:
    ok(sha(BASE/r['path'])==r['sha256'],'r2 source preimage '+r['path'])
    if sha(COMP/r['path'])!=r['sha256']:changed.append(r['path'])
ok(set(changed)=={'host/bridge.cpp','scripts/build_host.py','scripts/compile_dos.py','src/src/sbbs3/dosxtrn/dosxtrn.c'},'only four original component inputs changed')
old=json.loads((ROOT/'build/sfxexec-components-r8/build.json').read_text())
for r in old['source']:
    if r['path']=='src/src/sbbs3/dosxtrn/dosxtrn.c':continue
    ok(sha(COMP/r['path'])==r['sha256'],'unchanged core source '+r['path'])
for build in (HOST,PROBE):
    record=json.loads((build/'build.json').read_text())
    for r in record['source']:ok(sha(COMP/r['path'])==r['sha256'],'final compiled source '+r['path'])
aliases=json.loads((DOS/'short-header-map.json').read_text())
def short(t):return re.sub(r'(#\s*include\s*["<])([^">]+)([">])',lambda m:m[1]+str(Path(m[2]).with_name(aliases.get(Path(m[2]).name,Path(m[2]).name))).replace('\\','/')+m[3],t)
ok((DOS/'drive/SRC/DOSXTRN/SFXDOOR.C').read_text()==short((COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text()),'DOS compiler staged exact current source')
for p in (COMP/'src/src').rglob('*.h'):
    if p.name not in aliases:continue
    q=DOS/'drive'/('INC' if p.parent.name=='xpdev' else 'SRC')/aliases[p.name];ok(q.read_text()==short(p.read_text()),'DOS actual header '+p.name)
images=[]
for p in (CORE,DOS/'drive/SRC/DOSXTRN/SFXDOOR.EXE',HOST/'SFXDOSHost.exe',PROBE/'SFXLDR.DLL'):
    data=p.read_bytes();ok(data[:2]==b'MZ','MZ '+p.name)
    if p.name=='SFXDOOR.EXE':
        words=struct.unpack_from('<14H',data);header=words[4]*16
        ok((words[2]-1)*512+(words[1] or 512)==len(data),'DOS exact MZ length')
        ok(words[12]+words[3]*4<=header and header+words[11]*16+words[10]<len(data),'DOS relocation/entry ranges')
        for n in range(words[3]):off,seg=struct.unpack_from('<HH',data,words[12]+n*4);ok(header+seg*16+off+2<=len(data),'DOS relocation within image')
        ok(data.count(b'\xc4\xc4\x58\x00')==2,'only two direct BOP register instructions: core and diagnostic')
    else:
        pe=struct.unpack_from('<I',data,60)[0];opt=pe+24;ok(data[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',data,pe+4)[0]==0x14c,'PE32 x86 '+p.name)
        ok(struct.unpack_from('<H',data,opt)[0]==0x10b and struct.unpack_from('<HH',data,opt+48)==(6,1),'Win7 subsystem6.01 '+p.name)
        if p.name=='SFXLDR.DLL':
            ok(struct.unpack_from('<I',data,opt+16)[0]==0,'diagnostic no entry point')
            dump=(PROBE/'pe.txt').read_text();mods=re.findall(r'^    ([A-Za-z0-9_.]+\.dll)\s*$',dump,re.M|re.I)
            ok([m.lower() for m in mods]==['kernel32.dll'],'diagnostic kernel32 imports only; no CRT/static ntvdm')
    images.append(dict(name=p.name,bytes=len(data),sha256=sha(p)))
ok(sha(CORE)=='D36589BDE11134B7F6019C32667B639BC465CA39CD39E22FEB6E053A2996A3F3','core DLL exact r2')
source=(COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text();failed=source[source.index('if(!success) {'):source.index('#if 0',source.index('if(!success) {'))]
ok('sfx_load_diagnose(dll);return 18;' in failed,'failed BOP always returns before core open/game')
ok('DllName=VDD_FILENAME;' not in source and 'SBBSEXEC.DLL' not in source,'no bare-name or Sync fallback')
ok('init=0000:0000' in source and 'not a Win32 error' in source,'BOP fields distinct from Win32 diagnostics')
for name,h in [('SFX-1.0.0-alpha.2-SFXEXEC-Component-Test-20261024-r2.zip','230EF851A9B112045CD5A5E9B5065301D1200D669811CBDA51A9F5DD4BE089D7'),('SFX-1.0.0-alpha.2-SFXEXEC-Component-Source-20261024-r2.zip','1BA426F8A80FF33C0B7ACFAC33C5D1160B7A79E5F66E343421AD26FE3B6532A8'),('SFX-1.0.0-alpha.2-Section11-OwnerFixes-Windows-Owner-Test-20261024-r1.zip','CE56C08B3133AE8E0D31ED5C6000CA73DB4C2689E653F59ECD85CC3F420085D4'),('SFX-1.0.0-alpha.2-Section11-OwnerFixes-Engineering-Companion-20261024-r1.zip','FDFAEBCF86F6B04E8A3BCC6F93B5C9313D041F09F6F6904DEBFE217999BEBD4C')]:ok(sha(ROOT/'build'/name)==h,'old archive retained '+name)
accepted=ROOT/'build/section11-doors-actions-product-r3'
for r in json.loads((accepted/'source-inputs-before.json').read_text()):ok(sha(accepted/'source-at-start'/r['path'])==r['sha256'],'accepted830 preimage retained '+r['path'])
queued=json.loads((ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024/queued-category-source.json').read_text())
for r in queued['changes']:ok(sha(ROOT/r['path'])==r['after'],'queued R246 source unchanged '+r['path'])
ok(sha(ROOT/'test/integration/door_first_run_defaults.inc')==queued['new_include_sha256'],'queued R246 include unchanged')
files=[dict(path=p.relative_to(COMP).as_posix(),bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(COMP.rglob('*')) if p.is_file()]
binding=hashlib.sha256(json.dumps(files,sort_keys=True).encode()).hexdigest().upper()
out=dict(result='PASS static/source/preservation; native loader cause PENDING',checks=len(checks),images=images,files=files,binding_sha256=binding,changed_r2_files=changed,
    new_files=[r['path'] for r in files if not (BASE/r['path']).exists()],inherited_core_fixture_checks=5470,new_probe_checks=70,new_pe_checks=7,new_host_helper_checks=25,host_model_checks=80,affected_rlogin_checks=56)
(EV/'inspection-r3.json').write_text(json.dumps(out,indent=2)+'\n');print('PASS',len(checks),'loader static/source/preservation checks; native cause OPEN')
