"""Matched component/source candidate only; no installer/tools/owner game data."""
from pathlib import Path
import subprocess,sys,hashlib,json,zipfile,shutil,re
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';EV=ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024'
BUILD=ROOT/'build/sfxexec-delivery-r2';assert not BUILD.exists(),'fresh delivery only';BUILD.mkdir()
DLL=ROOT/'build/sfxexec-components-r8';DOS=ROOT/'build/sfxexec-dos-compile-r7';HOST=ROOT/'build/sfxexec-host-r4'
OWNER_NAME='SFX-1.0.0-alpha.2-SFXEXEC-Component-Test-20261024-r2.zip'
SOURCE_NAME='SFX-1.0.0-alpha.2-SFXEXEC-Component-Source-20261024-r2.zip'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
def write(p,text):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text,encoding='utf-8',newline='\n')
def copy(p,q):q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
subprocess.run([sys.executable,str(COMP/'scripts/inspect_candidate.py')],cwd=ROOT,check=True)
inspection=json.loads((EV/'candidate-inspection.json').read_text());source_binding=hashlib.sha256(json.dumps(inspection['source'],sort_keys=True).encode()).hexdigest().upper()
fixture=json.loads((ROOT/'build/sfxexec-vdd-fixture-r9/result.json').read_text());assert fixture['candidate_sha256']==sha(DLL/'SFXEXEC.DLL')
new_checks=sum(int(re.search(r'PASS (\d+)',r['log'])[1]) for r in fixture['tests'])
def stage_source(root):
    for p in COMP.rglob('*'):
        if p.is_file():copy(p,root/'components/sfxexec'/p.relative_to(COMP))
    # Complete source, not proprietary headers/tool media. Build-only host
    # metadata makes the private compiler recipe relocatable after reacquisition.
    copy(EV/'compiler-host.json',root/'research/architecture/evidence/section11-sfxexec-components-20261024/compiler-host.json')
    write(root/'SOURCE-MANIFEST.json',json.dumps(dict(binding_sha256=source_binding,files=inspection['source']),indent=2)+'\n')
def identities(root):
    for p,name in [(DLL/'SFXEXEC.DLL','SFXEXEC.DLL'),(DOS/'drive/SRC/DOSXTRN/SFXDOOR.EXE','SFXDOOR.EXE'),(HOST/'SFXDOSHost.exe','SFXDOSHost.exe')]:copy(p,root/'BIN'/name)
    write(root/'BIN-IDENTITY.txt',''.join(r['sha256']+'  BIN/'+r['name']+'  ('+str(r['bytes'])+' bytes)\n' for r in inspection['images'])+'\nAccepted Runtime SHA256: 90E2F01DE0BBD018830E30EC58CD3703710A769A286DB1FB39C06A5780AE0C8F\nSource binding: '+source_binding+'\n')
def manifest(root):
    files=sorted(p for p in root.rglob('*') if p.is_file());write(root/'SHA256SUMS.txt',''.join(sha(p)+'  '+p.relative_to(root).as_posix()+'\n' for p in files))
def pack(root,name):
    archive=ROOT/'build'/name;assert not archive.exists()
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for p in sorted(root.rglob('*')):
            if not p.is_file():continue
            info=zipfile.ZipInfo(root.name+'/'+p.relative_to(root).as_posix(),(2026,10,24,10,0,0));info.compress_type=zipfile.ZIP_DEFLATED;info.external_attr=0o100644<<16;z.writestr(info,p.read_bytes())
    return dict(archive=str(archive),bytes=archive.stat().st_size,sha256=sha(archive),files=sum(p.is_file() for p in root.rglob('*')))
source=BUILD/'SFXDOS-Source';stage_source(source);identities(source/'Candidate')
write(source/'README.txt','Corresponding source and engineering receipts for SFXEXEC component test20261024-r2. Use the matched Component-Test ZIP for complete owner-run profiles/start scripts. Candidate/BIN contains the identical three images for correspondence, not a second installer. See components/sfxexec/REBUILD.txt and ENGINEERING-REPORT.txt.\n')
for p in (COMP/'delivery').glob('*'):copy(p,source/'Candidate'/p.name)
evidence=source/'Engineering'
for p in [EV/'candidate-inspection.json',EV/'tool-provenance-verified.json',EV/'private-tool-extraction.json',EV/'queued-category-source.json',EV/'compiler-host.json',EV/'BRIDGE_SCOPE_REVIEW.txt',EV/'CR4_SFX_DOS_DLL_LAUNCHER_HANDOFF_APPROVED_20261024.txt',
    DLL/'build.json',DOS/'build.json',HOST/'build.json']:
    prefix='dll-' if p.parent==DLL else 'dos-' if p.parent==DOS else 'host-' if p.parent==HOST else ''
    copy(p,evidence/(prefix+p.name))
for folder,label in [(DLL,'DLL'),(HOST,'Host'),(ROOT/'build/sfxexec-vdd-fixture-r9','Fixture'),(ROOT/'build/sfxexec-regressions-r1','Regressions')]:
    for p in folder.rglob('*'):
        if p.is_file() and (p.suffix.lower() in {'.log','.txt'} or p.name=='result.json'):copy(p,evidence/label/p.relative_to(folder))
for p in [DOS/'drive/COMPILE.LOG',DOS/'host.log',DOS/'short-header-map.json',DOS/'drive/SRC/DOSXTRN/SFXDOOR.MAP',DOS/'drive/SRC/DOSXTRN/SFXDOOR.COD',HOST/'pair_hashes.hpp']:
    assert p.is_file(),p;copy(p,evidence/'Build-Details'/p.name)
# Retain failure receipts as TEXT only; no fake ntvdm/harness EXE or tool payload.
for label in ('sfxexec-vdd-fixture-r6','sfxexec-vdd-fixture-r7'):
    for p in (ROOT/'build'/label).glob('*.log'):copy(p,evidence/'Retained-Failures'/label/p.name)
copy(EV/'legacy-cl-probe.log',evidence/'Retained-Failures/legacy-cl-probe.log')
notice='This product includes software developed by HD Associates, Inc\nThe preserved xpsem.h source notice is included in full in SOURCE/components/sfxexec/upstream/src/xpdev/xpsem.h (or components/sfxexec/upstream in the source ZIP). Its Unix-only branch is not in the actual Windows compiler include closure or linked candidate. Original notices and license texts remain intact.\n'
write(source/'SOURCE-REVIEW.txt','PASS bounded source/dependency/notice review: original44 files retained; actual11 C units and include closure hash-bound; GPL/LGPL texts and modified-file notices supplied. XPDEV Windows branch used. System Microsoft headers/SDK/CRT are private tool dependencies; their media/files are excluded. Independent SFX host links system libraries only.\n'+notice)
report=f'''SFXEXEC COMPONENT CANDIDATE 2026-10-24-r2
Local DLL/launcher/bridge build and bounded engineering checks COMPLETE.
Native DOS/VDD loading, Win7 execution and SFX+Sync coexistence PENDING owner test.
DLL actual dispatcher/Win32 IPC: PASS {new_checks} checks in two concurrent x86 processes using the SAME visible node123 and distinct nonce/bytes. CPU/interrupt services are mocked.12 rounds/process including startup refusal, byte-range/node binding, late data, pressure and exact resource cleanup. This is not native FOSSIL/UART/Sync qualification.
Bridge: PASS80 model checks and x64 early refusal. Independent source links no SFX/GPL implementation. Native VDM/job/loopback lifecycle remains PENDING.
Accepted unchanged regression binaries: PASS19690 Door-session,7167 Door/RLogin,56 native loopback RLogin checks. No external BBS or owner install accessed.
Static/source/image/preservation: PASS {inspection['checks']} checks. Actual dependency/header closure and per-image imports/exports retained. DLL/host PE32 x86 subsystem6.01; launcher true DOS MZ, real BOP,20/19-byte ABI. No identified post-Win7 direct imports; native Win7 remains pending.
Toolchain: MSVC14.44.35207/SDK10.0.26100.0 for x86 VDD/host; original MSVC1.52c CL8.00c + Link5.60.339 for DOS launcher. A verified private DOSBox-X ran only the compiler after native legacy compiler failures. No game/emulator runtime is shipped or qualified.
Earlier DLL/staging/fixture failures retained locally; representative text receipts included. First ACL conversion's15 one-time Windows handles were reproduced with independent public SID/SDDL APIs before taking the resource baseline; every candidate round then retained zero handles.
Strict private loader path only; actual Win10/32 module load PENDING. No System32 copy or Sync change. Game DTR whole-caller disconnect disabled in first bridge profile; operator controls retained through accepted gateway and pending native observation. No broad local-Door engine/schema/Setup work.
R244 accepted830 frozen inputs/media and queued R246 starter-category source verified intact. Categories excluded. All previous source/media retained; no backup/publication/Master completion/owner access.
Source binding {source_binding}
STOP for DR/owner review and owner-controlled Win10/32 same-node matrix.
'''
write(source/'ENGINEERING-REPORT.txt',report);manifest(source);source_record=pack(source,SOURCE_NAME)
owner=BUILD/'SFXDOS';identities(owner)
for p in (COMP/'delivery').glob('*'):copy(p,owner/p.name)
stage_source(owner/'SOURCE');write(owner/'SOURCE-NOTICES.txt',(COMP/'SFX-CHANGES.txt').read_text()+'\n'+notice)
write(owner/'ENGINEERING-REPORT.txt',report)
write(owner/'SOURCE-AVAILABILITY.txt',SOURCE_NAME+'\nSHA256 '+source_record['sha256']+'\nComplete source is also included in SOURCE. Matching source ZIP adds engineering receipts; no source rights are removed.\n')
for game,profile,port,mode,drop,command,name in [('OUTPOST','outpost',15131,'fossil','doorsys','OTRADER.EXE /D{NODEDIR}','SFX DOS - Outpost'),('SFGUESS','sfguess',15132,'uart','sfdoors','SFGUESS.EXE','SFX DOS - SF Guess')]:
    write(owner/'PROFILES'/(profile+'.ini'),f'# Designated test account; fill caller_alias before running.\nsfx_root=C:\\SFX\ncaller_alias=CHANGE_ME\nprofile={profile}\nlisten_port={port}\ndoor_name={name}\nwork_directory=C:\\SFXDOS\\GAMES\\{game}\ncommand={command}\nmode={mode}\ndropfile={drop}\nsecurity_level=10\nminutes=5\nsysop_name=Sysop\nlocation=Designated test account\n')
    label='Outpost' if profile=='outpost' else 'SFGuess'
    cmd='@echo off\r\ncd /d "%~dp0"\r\nif not exist LOGS mkdir LOGS\r\npowershell.exe -NoLogo -NoProfile -Command "$taskLog=\'.\\LOGS\\'+profile+'-\'+(Get-Date -Format yyyyMMdd-HHmmss-fff)+\'.log\'; if(Test-Path -LiteralPath $taskLog){throw \'Existing log retained\'}; & \'.\\BIN\\SFXDOSHost.exe\' --profile \'.\\PROFILES\\'+profile+'.ini\' 2>&1 | Tee-Object -FilePath $taskLog"\r\npause\r\n'
    (owner/('Start-'+label+'.cmd')).write_bytes(cmd.encode('ascii'))
write(owner/'GAMES/README.txt','Owner supplies independent game copies here. No game/state/account is bundled. See README-FIRST.txt.\n')
manifest(owner);owner_record=pack(owner,OWNER_NAME)
# Both streams are reopened, CRC checked, extracted to fresh paths and byte
# compared to staged files. Independent native .NET stream verification follows.
for root,record in [(source,source_record),(owner,owner_record)]:
    extraction=BUILD/('Verified-'+root.name);extraction.mkdir()
    with zipfile.ZipFile(record['archive']) as z:
        assert z.testzip() is None
        for n in z.namelist():
            assert not n.startswith('/') and '..' not in Path(n).parts
            p=root.parent/n;assert p.is_file() and z.read(n)==p.read_bytes(),n
            assert Path(n).name=='COPYING.LIB' or not n.lower().endswith(('.lib','.obj','.iso','.7z','.db','.sqlite','.pfx','.pem')),n
            if n.lower().endswith(('.exe','.dll')):assert n.split('/')[-1] in {'SFXEXEC.DLL','SFXDOOR.EXE','SFXDOSHost.exe'},n
        z.extractall(extraction)
        for n in z.namelist():assert sha(extraction/n)==sha(root.parent/n),n
    record.update(extraction=str(extraction),python_everybyte='PASS',native_zip_streams='PENDING')
result=dict(result='READY candidate; native owner tests PENDING',owner=owner_record,source=source_record,source_binding_sha256=source_binding,
    final_dll_fixture_checks=new_checks,bridge_model_checks=80,regression_checks=26913,static_checks=inspection['checks'],images=inspection['images'],no_setup=True,no_owner_execution=True,coexistence='PENDING')
(EV/'delivery-r2.json').write_text(json.dumps(result,indent=2)+'\n')
write(ROOT/'build/SFXEXEC-Component-Test-20261024-r2-SHA256SUMS.txt',''.join(r['sha256']+'  '+Path(r['archive']).name+'\n' for r in (owner_record,source_record)))
print(json.dumps(result,indent=2))
