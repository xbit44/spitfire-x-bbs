"""Finite diagnostic update with complete source; never touches owner files."""
from pathlib import Path
import json,hashlib,subprocess,sys,shutil,zipfile,re,difflib
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';EV=ROOT/'research/architecture/evidence/section11-sfxexec-loader-fix-20261024'
BUILD=ROOT/'build/sfxexec-loader-delivery-r4';assert not BUILD.exists();BUILD.mkdir()
CORE=ROOT/'build/sfxexec-components-r8/SFXEXEC.DLL';DOS=ROOT/'build/sfxexec-dos-compile-loader-r2';HOST=ROOT/'build/sfxexec-host-loader-r3';PROBE=ROOT/'build/sfxexec-loader-probe-r1'
OWNER_NAME='SFX-1.0.0-alpha.2-SFXEXEC-Loader-Diagnostic-Test-20261024-r4.zip'
SOURCE_NAME='SFX-1.0.0-alpha.2-SFXEXEC-Loader-Diagnostic-Source-20261024-r4.zip'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def write(p,t):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(t,encoding='utf-8',newline='\n')
subprocess.run([sys.executable,str(COMP/'scripts/inspect_loader_candidate.py')],cwd=ROOT,check=True)
inspection=json.loads((EV/'inspection-r3.json').read_text());binding=inspection['binding_sha256']
fixtures=json.loads((ROOT/'build/sfxexec-loader-fixture-r1/result.json').read_text());ok_host=json.loads((ROOT/'build/sfxexec-host-loader-fixture-r2/result.json').read_text())
assert fixtures['core_sha256']==sha(CORE) and fixtures['probe_sha256']==sha(PROBE/'SFXLDR.DLL')
assert ok_host['host_source_sha256']==sha(COMP/'host/bridge.cpp')
def source_tree(root):
    for p in COMP.rglob('*'):
        if p.is_file():copy(p,root/'components/sfxexec'/p.relative_to(COMP))
    for p in (EV/'baseline-source').rglob('*'):
        if p.is_file():copy(p,root/'components/sfxexec/baseline-r2'/p.relative_to(EV/'baseline-source'))
    prior=ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024/compiler-host.json';copy(prior,root/'research/architecture/evidence/section11-sfxexec-components-20261024/compiler-host.json')
    write(root/'SOURCE-MANIFEST.json',json.dumps(dict(binding_sha256=binding,files=inspection['files']),indent=2)+'\n')
def bins(root):
    for p in (CORE,DOS/'drive/SRC/DOSXTRN/SFXDOOR.EXE',HOST/'SFXDOSHost.exe',PROBE/'SFXLDR.DLL'):copy(p,root/'BIN'/p.name)
    write(root/'BIN-IDENTITY.txt','SFXEXEC LOADER DIAGNOSTIC 20261024-r4; native cause OPEN\n\n'+''.join(r['sha256']+'  BIN/'+r['name']+'  ('+str(r['bytes'])+' bytes)\n' for r in inspection['images'])+'\nCore DLL remains byte-identical to r2. All four files are required by this host.\nSource binding '+binding+'\n')
def manifests(root):write(root/'SHA256SUMS.txt',''.join(sha(p)+'  '+p.relative_to(root).as_posix()+'\n' for p in sorted(root.rglob('*')) if p.is_file()))
def pack(root,name):
    path=ROOT/'build'/name;assert not path.exists()
    with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for p in sorted(root.rglob('*')):
            if not p.is_file():continue
            item=zipfile.ZipInfo(root.name+'/'+p.relative_to(root).as_posix(),(2026,10,24,16,0,0));item.compress_type=zipfile.ZIP_DEFLATED;item.external_attr=0o100644<<16;z.writestr(item,p.read_bytes())
    return dict(archive=str(path),bytes=path.stat().st_size,sha256=sha(path),files=sum(p.is_file() for p in root.rglob('*')))
report=f'''SFXEXEC LOADER r4 DIAGNOSTIC CANDIDATE - 2026-10-24
Native failure cause remains OPEN. One instrumented owner-controlled SF Guess retry is needed. No verified loader fix or new native pass is claimed.
Exact r2 source and six owner-evidence files verified locally before changes; raw owner evidence/profile is NOT shipped. AX1 is the RegisterModule BOP result, not Win32 GetLastError1. Failure occurs before pair/open/game. Upstream/r2 register conventions compared; no proven ABI/pointer cause established.
Core SFXEXEC.DLL remains D36589BDE11134B7F6019C32667B639BC465CA39CD39E22FEB6E053A2996A3F3. Its5470 dispatcher/Win32 IPC fixture checks are INHERITED on identical bytes, not rerun or native DOS evidence.
NEW: PASS70 actual diagnostic-DLL checks across five ordinary-process/fake-NTVDM fixtures: valid mapping, absent target, wrong bitness193, missing NTVDM export127, present target/missing dependency126. PASS7 actual PE reader checks. These are NOT native NTVDM/BOP proof.
NEW: PASS25 production host helper/Win32 file/socket checks: missing/mismatched core/launcher/probe refusal, readonly pin, candidate-root-relative/absolute guess paths, SFDOORS game-working-directory placement, DOOR.SYS node placement, owned transient unwinding, collision preservation, non-Runtime peer refusal. No installed board/game touched.
NEW: PASS80 host model/x64 early-refusal checks; PASS56 exact accepted native loopback RLogin regression checks. Native VDM/job/coexistence still PENDING.
PASS{inspection['checks']} source/image/ABI/preservation checks: four old component inputs changed; original r2/older media/source and accepted830 frozen inputs retained; R246 queued starter changes exact/excluded. Original44 upstream files/notices retained. Complete core/probe/launcher/host source and r2 baseline supplied.
Modern MSVC14.44.35207/SDK10.0.26100.0; legacy launcher MSVC1.52c CL8.00c/Link5.60.339 under the previously verified compiler-only DOS host. Final launcher DOS build loader-r2 (loader-r1 compile failure retained); host loader-r3; diagnostic r1. Diagnostic PE32 x866.01 has no entry point, CRT or static NTVDM imports; kernel32 only. Core/host PE32 x866.01; genuine DOS MZ/BOP with compile-asserted wire/status/FOSSIL sizes. Native Win7 loading PENDING.
Strict full paths, matched hashes/file pins, actual core module identity and private node/session IPC retained. A failed core BOP invokes only the private diagnostic, never another core path/game. If probe BOP also fails, Win32 detail is unavailable and reported honestly. Readonly native-process metadata is not proof of registration.
No System32 placement proposed/performed. No full Setup/local-Door engine/UI/schema/node-manager/INI/YieldInterval work. No owner/prod access, Sync modification, ordinary-caller interference, cleanup/backup/publication/Master/subagent/callback. STOP for DR review and one owner native attempt.
Source binding {binding}
'''
source=BUILD/'SFXLDR-SOURCE';source_tree(source);bins(source/'Candidate')
write(source/'ENGINEERING-REPORT.txt',report);write(source/'README.txt','Corresponding source/engineering evidence for the matched SFXEXEC Loader Diagnostic Test20261024-r4 ZIP. Use that Test ZIP for owner update/retest instructions. Read components/sfxexec/LOADER-REBUILD.txt. Candidate/BIN has identical images for correspondence. No owner games/profiles/data or tool/media binaries.\n')
for p in (COMP/'loader-delivery').glob('*'):copy(p,source/'Candidate'/p.name)
e=source/'Engineering'
copy(EV/'inspection-r3.json',e/'inspection-r3.json')
for folder,label in [(HOST,'Host'),(PROBE,'Probe'),(ROOT/'build/sfxexec-loader-fixture-r1','Probe-Fixtures'),(ROOT/'build/sfxexec-host-loader-fixture-r2','Host-Fixtures'),(ROOT/'build/sfxexec-loader-regressions-r1','RLogin')]:
    for p in folder.rglob('*'):
        if p.is_file() and (p.suffix.lower() in {'.log','.txt'} or p.name in {'build.json','result.json','pair_hashes.hpp'}):copy(p,e/label/p.relative_to(folder))
for p in [DOS/'build.json',DOS/'short-header-map.json',DOS/'host.log',DOS/'drive/COMPILE.LOG',DOS/'drive/SRC/DOSXTRN/SFXDOOR.COD',DOS/'drive/SRC/DOSXTRN/SFXDOOR.MAP']:copy(p,e/'DOS'/p.name)
for p in (ROOT/'build/sfxexec-dos-compile-loader-r1/drive').glob('COMPILE.LOG'):copy(p,e/'Retained-Failure/loader-r1-COMPILE.LOG')
for path in inspection['changed_r2_files']:
    before=(EV/'baseline-source'/path).read_text();after=(COMP/path).read_text();write(e/'Diffs'/(path.replace('/','_')+'.diff'),''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='r2/'+path,tofile='r3/'+path)))
copy(COMP/'COPYING',source/'COPYING');copy(COMP/'COPYING.LIB',source/'COPYING.LIB')
manifests(source);source_record=pack(source,SOURCE_NAME)
owner=BUILD/'SFXLDR';bins(owner)
for p in (COMP/'loader-delivery').glob('*'):copy(p,owner/p.name)
copy(COMP/'delivery/Verify-Package.ps1',owner/'Verify-Package.ps1');source_tree(owner/'SOURCE');write(owner/'ENGINEERING-REPORT.txt',report)
write(owner/'SOURCE-AVAILABILITY.txt',SOURCE_NAME+'\nSHA256 '+source_record['sha256']+'\nComplete corresponding source is also in SOURCE; the matching Source ZIP adds engineering receipts. GPL/LGPL rights/notices retained.\n')
notice='External core/launcher/probe source retains GPL-2.0-or-later; XPDEV LGPL notices/texts remain. Original source notices are complete under SOURCE. This product includes software developed by HD Associates, Inc (preserved Unix-only xpsem.h source, not linked into the Windows candidate). Independent SFX host source and existing rights retained. No compiler/DDK/SDK media ships.\n'
write(owner/'SOURCE-NOTICES.txt',notice)
for mode,game,profile,port,name,command,drop in [('uart','guess','sfguess',15132,'SFX DOS - SF Guess','SFGUESS.EXE','sfdoors'),('fossil','outpost','outpost',15131,'SFX DOS - Outpost','OTRADER.EXE /D{NODEDIR}','doorsys')]:
    write(owner/'EXAMPLES'/(profile+'.example.ini'),f'# EXAMPLE ONLY. Preserve your working profile; do not replace it blindly.\nsfx_root=D:\\SFX\ncaller_alias=CHANGE_ME\nprofile={profile}\nlisten_port={port}\ndoor_name={name}\nwork_directory=GAMES\\{game}\ncommand={command}\nmode={mode}\ndropfile={drop}\nsecurity_level=10\nminutes=5\nsysop_name=Sysop\nlocation=Designated test account\n')
manifests(owner);owner_record=pack(owner,OWNER_NAME)
allowed={r['name']:r['sha256'] for r in inspection['images']}
for root,record in [(owner,owner_record),(source,source_record)]:
    extracted=BUILD/('Verified-'+root.name);extracted.mkdir()
    with zipfile.ZipFile(record['archive']) as z:
        assert z.testzip() is None
        for n in z.namelist():
            assert '..' not in Path(n).parts and not n.startswith('/');p=root.parent/n;assert p.is_file() and z.read(n)==p.read_bytes(),n
            assert Path(n).name=='COPYING.LIB' or not n.lower().endswith(('.lib','.obj','.iso','.7z','.db','.sqlite','.pfx','.pem','.png','.mp4')),n
            if n.lower().endswith(('.exe','.dll')):assert Path(n).name in allowed and sha(p)==allowed[Path(n).name],n
            assert not n.lower().endswith('sfdoors.dat') and not '/profiles/' in n.lower(),n
        z.extractall(extracted)
        for n in z.namelist():assert sha(extracted/n)==sha(root.parent/n),n
    record.update(extraction=str(extracted),python_everybyte='PASS',native_zip_streams='PENDING')
out=dict(result='DIAGNOSTIC READY; native loader cause OPEN / retest PENDING',owner=owner_record,source=source_record,source_binding_sha256=binding,
    images=inspection['images'],static_checks=inspection['checks'],new_probe_checks=70,new_pe_checks=7,new_host_checks=25,host_model_checks=80,new_rlogin_checks=56,inherited_unchanged_core_checks=5470,
    no_native_success=True,no_owner_access=True,no_system_placement=True)
(EV/'delivery-r4.json').write_text(json.dumps(out,indent=2)+'\n')
write(ROOT/'build/SFXEXEC-Loader-Diagnostic-20261024-r4-SHA256SUMS.txt',''.join(r['sha256']+'  '+Path(r['archive']).name+'\n' for r in (owner_record,source_record)))
print(json.dumps(out,indent=2))
