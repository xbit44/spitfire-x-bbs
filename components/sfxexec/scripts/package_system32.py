"""Matched finite System32 component/source delivery; no system installation."""
from pathlib import Path
import json,hashlib,subprocess,sys,shutil,zipfile,difflib
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
EV=ROOT/'research/architecture/evidence/section11-sfxexec-system32-20261024'
BUILD=ROOT/'build/sfxsys-r2';assert not BUILD.exists();BUILD.mkdir()
CORE=ROOT/'build/sfxexec-components-r8/SFXEXEC.DLL';DOS=ROOT/'build/sfxexec-dos-compile-system32-r1';HOST=ROOT/'build/sfxexec-host-system32-r4';FIX=ROOT/'build/sfxexec-system32-fixture-r3'
TEST='SFX-1.0.0-alpha.2-SFXEXEC-System32-Test-20261024-r2.zip'
SOURCE='SFX-1.0.0-alpha.2-SFXEXEC-System32-Source-20261024-r2.zip'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
def write(p,t):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(t,encoding='utf-8',newline='\n')
subprocess.run([sys.executable,str(COMP/'scripts/inspect_system32.py')],cwd=ROOT,check=True)
inspect=json.loads((EV/'inspection-r1.json').read_text());binding=inspect['binding_sha256']
def tree(root):
    for r in inspect['files']:copy(COMP/r['path'],root/'components/sfxexec'/r['path'])
    for folder,label in [(EV/'baseline-r4','baseline-r4'),(ROOT/'research/architecture/evidence/section11-sfxexec-loader-fix-20261024/baseline-source','baseline-r2')]:
        for p in folder.rglob('*'):
            if p.is_file():copy(p,root/'components/sfxexec'/label/p.relative_to(folder))
    copy(ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024/compiler-host.json',root/'research/architecture/evidence/section11-sfxexec-components-20261024/compiler-host.json')
    write(root/'SOURCE-MANIFEST.json',json.dumps(dict(binding_sha256=binding,files=inspect['files']),indent=2)+'\n')
def candidate(root):
    for p in (DOS/'drive/SRC/DOSXTRN/SFXDOOR.EXE',HOST/'SFXDOSHost.exe'):copy(p,root/'BIN'/p.name)
    copy(CORE,root/'SYSTEM32/SFXEXEC.DLL')
    write(root/'BIN-IDENTITY.txt','SFXEXEC System3220261024-r2 - STOP for DR review before placement\n\n'+''.join(r['sha256']+'  '+('SYSTEM32/' if r['name']=='SFXEXEC.DLL' else 'BIN/')+r['name']+'  ('+str(r['bytes'])+' bytes)\n' for r in inspect['images'])+'\nSource binding '+binding+'\nCore unchanged r2/r4. BIN contains ONLY launcher and host. SYSTEM32 is a package source folder, not an install action.\n')
    for p in (COMP/'system32-delivery').glob('*'):copy(p,root/p.name)
    copy(COMP/'delivery/Verify-Package.ps1',root/'Verify-Package.ps1')
def manifest(root):write(root/'SHA256SUMS.txt',''.join(sha(p)+'  '+p.relative_to(root).as_posix()+'\n' for p in sorted(root.rglob('*')) if p.is_file()))
def pack(root,name):
    path=ROOT/'build'/name;assert not path.exists()
    with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for p in sorted(root.rglob('*')):
            if not p.is_file():continue
            entry=zipfile.ZipInfo(root.name+'/'+p.relative_to(root).as_posix(),(2026,10,24,20,0,0));entry.compress_type=zipfile.ZIP_DEFLATED;entry.external_attr=0o100644<<16;z.writestr(entry,p.read_bytes())
    return dict(archive=str(path),bytes=path.stat().st_size,sha256=sha(path),files=sum(p.is_file() for p in root.rglob('*')))
report=f'''SFXEXEC SYSTEM32 ENGINEERING CANDIDATE -20261024-r2
STOP for DR review BEFORE any real System32 placement/native owner tests.
Fresh bounded owner System32 GO and direct CR4 reconfirmation authorized local implementation/qualification/delivery. No owner/production/native VDM access or real System32 write occurred. Native cause remains OPEN; no verified native fix, Windows7 pass or same-node coexistence acceptance is claimed.

CHANGE
Core DLL remains exact r2/r4: D36589BDE11134B7F6019C32667B639BC465CA39CD39E22FEB6E053A2996A3F3. Original ops0..19 and private20/21/pair/session/actual-node/credits are unchanged. New launcher uses one exact SFXEXEC.DLL basename RegisterModule attempt with upstream NT null init convention, retaining DS:SI readback/BOP AX diagnostics; no probe or alternate driver fallback. Official WWIV/GameSrv installation docs prescribe System32 for the upstream DLL; pinned upstream launcher retries without path for Vista. This is evidence for the candidate mechanism, not native SFX proof.
https://docs.wwivbbs.org/en/latest/install/win/
https://www.gamesrv.ca/docs/
Pinned upstream97fab8d84a68882d1ceff0bed7cc05497f4d5973, original launcher line567.

Host resolves GetSystemDirectoryW after x64 refusal. It pins exact expected driver and matched launcher, rejects stale adjacent BIN driver, verifies reported loaded path using canonical handle path/file ID/hash and independently checks the actual owned VDM module inventory. Same-byte different-path and hardlink alternatives refuse. No System32 path/PATH/environment override. Before caller ACK, send SFXSTART2|session|node on existing bounded private input. Launcher consumes the exact token BEFORE game handlers/spawn and returns SFXSTARTED2|session|node. Missing/wrong/stale/cancelled token refuses. Game cannot start on the reviewed matched launcher before module verification. Startup acknowledgment queued just before immediate game exit is retained. No new engine/schema/opcode/IPC namespace/event or normal relay/credit/yield policy change. DTR CanDisconnect=false retained.

NEW QUALIFICATION
PASS {inspect['host_checks']} actual host helper/ordinary-process module/file checks: missing/different target or launcher, old adjacent copy, readonly pin, same-byte wrong location, reported versus actual module mismatch, wrong pair/session/node, repeated inventories without handle leaks, actual startup ACK queue behavior including immediate-exit race, retained SFDOORS placement.
PASS {inspect['gate_checks']} actual core/Win32 private IPC plus extracted ACTUAL launcher wait-function checks. Only16-bit SS:offset mapping is adapted to ordinary-process fake NTVDM registers; IRQ/CPU services mocked. Two concurrent processes use visible Node1, separate session tokens; valid/mismatched/stale/extra/oversize/partial/absent token, cancellation, timeout and exhausted ACK credit. Control bytes consumed before game permission; cleanup joined. Not native DOS/BOP/UART/FOSSIL/Sync proof.
PASS {inspect['placement_checks']} exact manual placement/rollback command-block checks in newly owned build fixtures only: absent create, exact reuse, conflicting existing/source mismatch/receipt refusal, replacement preservation, pending receipt refusal, normal file-in-use refusal and retained receipts. No automatic placement helper ships. Manual rollback has an explicit no-concurrent-change precondition and one ordinary deletion attempt.
PASS80 bridge model checks and x64 early refusal. PASS56 exact accepted localhost RLogin tests. Initial sandboxed RLogin handshake failed; retained log; exact unchanged binary passed with normal authorized local loopback access. No external/installed BBS accessed.
PASS{inspect['checks']} static/source/image/ABI/preservation checks. Original core5470 checks are INHERITED on byte-identical source/image, not rerun or converted into native passes. Prior diagnostic70/7/25 checks are historical only, not current qualification.

BUILD / SOURCE
Final DOS compile system32-r1: licensed MSVC1.52c CL8.00c/Link5.60.339 under verified private COMPILER-ONLY DOSBox-X. Actual MZ/one BOP/20-byte status/19-byte FOSSIL/16-bitint32-bitDWORD, .COD/.MAP supplied. Host system32-r4 MSVC14.44.35207/SDK10.0.26100.0 x86/MT/IA32/PE6.01. New direct APIs available by Win7; native Win7 remains pending. Earlier builds retained. First fixture compile had a test-only std::filesystem::copy name collision; failure/preimage retained, corrected before tests. No failed result labeled a pass.
Complete corresponding current component source, exact r2/r4 baseline source, original44 pinned inputs, notices and recipe are in BOTH ZIPs. Source binding {binding}. No GPL driver linked into independent host/SFX product libraries. No tool/media binaries, fake NTVDM/harness images, games, accounts, passwords, DBs, raw owner evidence or configured profiles distributed.

PRESERVATION / NATIVE BOUNDARY
Four old component inputs changed; other r4 files, r2/r4 media, accepted R244830 frozen inputs/media and queued/unshipped R246 starter edits preserved/excluded. No full Setup, normal Node-manager, global/per-Door INI/YieldInterval, engine/schema/UI/inbound/cleanup/backup/publication/Master/subagent/callback work.
Owner first target remains existing D:/SFXDOS and D:/SFX, GAMES/guess UART fresh SFDOORS.DAT; Outpost FOSSIL/DOOR.SYS52 follows first success. Exact one-file source/target/conflict/reuse/rollback and short native matrix are supplied. SFX Node1/Sync Node1 same-machine coexistence, actual time/operator/VDM behavior and Win7 are PENDING. Different names alone prove nothing. No automatic second candidate or native experiment.
'''
source=BUILD/'SFXSRC';tree(source);candidate(source/'Candidate');write(source/'ENGINEERING-REPORT.txt',report)
write(source/'README.txt','Matched source/engineering companion to '+TEST+'. Use Test ZIP instructions only after DR review. Full source baselines/current recipe in components/sfxexec; Candidate has the exact three matching binaries. No owner profiles or tool/media binaries.\n')
e=source/'Engineering';copy(EV/'inspection-r1.json',e/'inspection-r1.json')
for folder,label in [(HOST,'Host'),(FIX,'Fixtures')]:
    for p in folder.glob('*'):
        if p.is_file() and (p.suffix.lower() in {'.log','.txt'} or p.name in {'build.json','result.json','pair_hashes.hpp','launcher_gate.inc'}):copy(p,e/label/p.name)
for p in [DOS/'build.json',DOS/'short-header-map.json',DOS/'host.log',DOS/'drive/COMPILE.LOG',DOS/'drive/SRC/DOSXTRN/SFXDOOR.COD',DOS/'drive/SRC/DOSXTRN/SFXDOOR.MAP']:copy(p,e/'DOS'/p.name)
for name in ('placement-r1.log','rlogin.log','rlogin-r2.log'):copy(EV/name,e/name)
copy(ROOT/'build/sfxexec-system32-fixture-r1/SystemHostTests.exe.build.log',e/'Retained-Failures/fixture-r1-build.log')
for path in inspect['changed_r4_files']:
    write(e/'Diffs'/(path.replace('/','_')+'.diff'),''.join(difflib.unified_diff((EV/'baseline-r4'/path).read_text().splitlines(True),(COMP/path).read_text().splitlines(True),fromfile='r4/'+path,tofile='system32/'+path)))
for n in ('COPYING','COPYING.LIB'):copy(COMP/n,source/n)
manifest(source/'Candidate');manifest(source);source_record=pack(source,SOURCE)
owner=BUILD/'SFXSYS';candidate(owner);tree(owner/'SOURCE');write(owner/'ENGINEERING-REPORT.txt',report)
write(owner/'SOURCE-AVAILABILITY.txt',SOURCE+'\nSHA256 '+source_record['sha256']+'\nComplete corresponding current/r2/r4 source is also in SOURCE. The matching Source ZIP adds engineering receipts. GPL/LGPL rights/notices retained.\n')
write(owner/'SOURCE-NOTICES.txt','External core/launcher source retains GPL-2.0-or-later; XPDEV LGPL and original notices/texts remain. This product includes software developed by HD Associates, Inc (retained Unix-only xpsem.h source, not linked into Windows candidate). Independent host retains existing rights. No compiler/DDK/SDK/emulator media ships.\n')
for n in ('COPYING','COPYING.LIB'):copy(COMP/n,owner/n)
manifest(owner);owner_record=pack(owner,TEST)
allowed={r['name']:r['sha256'] for r in inspect['images']}
for root,record,label in [(owner,owner_record,'T'),(source,source_record,'S')]:
    extracted=BUILD/('V-'+label);extracted.mkdir()
    with zipfile.ZipFile(record['archive']) as z:
        assert z.testzip() is None;names=z.namelist();assert len({n.lower() for n in names})==len(names)
        for n in names:
            assert '..' not in Path(n).parts and not n.startswith('/');p=root.parent/n;assert p.is_file() and z.read(n)==p.read_bytes(),n
            assert len(str(extracted/n))<255,('MAX_PATH review',n)
            assert Path(n).name=='COPYING.LIB' or not n.lower().endswith(('.lib','.obj','.iso','.7z','.db','.sqlite','.pfx','.pem','.png','.mp4')),n
            if n.lower().endswith(('.exe','.dll')):assert Path(n).name in allowed and sha(p)==allowed[Path(n).name],n
            assert not n.lower().endswith('sfdoors.dat') and '/profiles/' not in n.lower(),n
        z.extractall(extracted)
        for n in names:assert sha(extracted/n)==sha(root.parent/n),n
    record.update(extraction=str(extracted),python_everybyte='PASS',native_zip_streams='PENDING')
out=dict(result='LOCAL CANDIDATE READY / STOP FOR DR REVIEW BEFORE PLACEMENT',owner=owner_record,source=source_record,source_binding_sha256=binding,images=inspect['images'],static_checks=inspect['checks'],host_checks=inspect['host_checks'],gate_checks=inspect['gate_checks'],placement_checks=28,model_checks=80,rlogin_checks=56,inherited_core_checks=5470,native_cause='OPEN',native_loading_and_coexistence='PENDING',real_system32_placement=False)
(EV/'delivery-r2.json').write_text(json.dumps(out,indent=2)+'\n')
write(ROOT/'build/SFXEXEC-System32-20261024-r2-SHA256SUMS.txt',''.join(r['sha256']+'  '+Path(r['archive']).name+'\n' for r in (owner_record,source_record)))
print(json.dumps(out,indent=2))
