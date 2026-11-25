"""Static identity/closure checks on the exact three candidate images."""
from pathlib import Path
import sys,json,hashlib,re,struct,zipfile
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec';EV=ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024'
DLL=ROOT/'build/sfxexec-components-r8';DOS=ROOT/'build/sfxexec-dos-compile-r7';HOST=ROOT/'build/sfxexec-host-r4'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
checks=[]
def ok(value,why):assert value,why;checks.append(why)
original=json.loads((COMP/'UPSTREAM.json').read_text())
for r in original['files']:ok(sha(COMP/'upstream'/r['path'])==r['sha256'],'original '+r['path'])
closure=[];dependencies=set()
for log in (DLL/'dll').glob('*.log'):
    for line in log.read_text(errors='replace').splitlines():
        if 'including file:' not in line:continue
        p=Path(line.split('including file:',1)[1].strip()).resolve();dependencies.add(p)
        if p.is_relative_to(COMP):ok(p.is_file(),'retained dependency '+str(p.relative_to(COMP)))
        else:ok(any(str(p).lower().startswith(str(root).lower()) for root in [Path('C:/Program Files (x86)/Windows Kits/10'),Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools')]),'system tool header only '+p.name)
for p in sorted(dependencies):closure.append(dict(path=str(p),sha256=sha(p),system_tool_file=not p.is_relative_to(COMP)))
record=json.loads((DLL/'build.json').read_text())
for r in record['source']:ok(sha(COMP/r['path'])==r['sha256'],'compiled DLL source exact '+r['path'])
aliases=json.loads((DOS/'short-header-map.json').read_text())
def short(text):return re.sub(r'(#\s*include\s*["<])([^">]+)([">])',lambda m:m[1]+str(Path(m[2]).with_name(aliases.get(Path(m[2]).name,Path(m[2]).name))).replace('\\','/')+m[3],text)
ok((DOS/'drive/SRC/DOSXTRN/SFXDOOR.C').read_text()==short((COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_text()),'actual DOS staged source matches canonical with documented8.3 includes')
for p in (COMP/'src/src').rglob('*.h'):
    if p.name not in aliases:continue
    q=DOS/'drive'/('INC' if p.parent.name=='xpdev' else 'SRC')/aliases[p.name]
    ok(q.read_text()==short(p.read_text()),'actual DOS staged header exact '+p.name)
for r in json.loads((HOST/'build.json').read_text())['source']:ok(sha(COMP/r['path'])==r['sha256'],'compiled bridge source exact '+r['path'])
images=[]
for p in [DLL/'SFXEXEC.DLL',HOST/'SFXDOSHost.exe']:
    data=p.read_bytes();pe=struct.unpack_from('<I',data,60)[0];ok(data[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',data,pe+4)[0]==0x14c,'PE32 x86 '+p.name)
    optional=pe+24;ok(struct.unpack_from('<H',data,optional)[0]==0x10b,'PE32 not PE32+ '+p.name)
    ok(struct.unpack_from('<HH',data,optional+48)==(6,1),'Win7 subsystem baseline '+p.name)
    dump=(DLL/'SFXEXEC-pe.txt' if p.suffix.lower()=='.dll' else HOST/'host-pe.txt').read_text()
    ok(not re.search(r'(?i)\b(?:VCRUNTIME\d*\.dll|MSVCP\d*\.dll|ucrtbase\.dll|api-ms-win-crt)',dump),'static CRT '+p.name)
    ok(not any(x in dump for x in ('GetSystemTimePreciseAsFileTime','WaitOnAddress','GetTempPath2','GetCurrentPackageId')),'no identified post-Win7 direct imports '+p.name)
    if p.suffix.lower()=='.dll':ok(re.findall(r'^\s+\d+\s+\d+\s+[0-9A-F]+\s+(VDD\w+)',dump,re.M)==['VDDDispatch','VDDInitialize'],'exact undecorated VDD exports')
    images.append(dict(name=p.name,bytes=len(data),sha256=sha(p)))
p=DOS/'drive/SRC/DOSXTRN/SFXDOOR.EXE';data=p.read_bytes();words=struct.unpack_from('<14H',data)
ok(words[0]==0x5a4d,'DOS MZ signature');last,pages,relocs,paragraphs=words[1:5];declared=(pages-1)*512+(last or 512)
ok(declared==len(data),'MZ exact declared file length');header=paragraphs*16
ok(header<=len(data) and words[12]+relocs*4<=header,'MZ relocation table range')
ok(header+words[11]*16+words[10]<len(data),'real-mode CS:IP in load image')
for i in range(relocs):off,seg=struct.unpack_from('<HH',data,words[12]+i*4);ok(header+seg*16+off+2<=len(data),'real-mode relocation within image')
ok(b'\xc4\xc4\x58' in data,'real Microsoft VDD BOP instruction emitted');ok(b'SFXEXEC.DLL' in data and b'sbbsexec.dll' not in data.lower(),'strict SFX DLL literal')
ok(b'NE'!=data[struct.unpack_from('<I',data,60)[0]:][:2] if len(data)>64 and struct.unpack_from('<I',data,60)[0]<len(data) else True,'not a Windows16 NE image')
images.append(dict(name=p.name,bytes=len(data),sha256=sha(p),relocations=relocs,entry_cs=words[11],entry_ip=words[10]))
queued=json.loads((EV/'queued-category-source.json').read_text())
for r in queued['changes']:ok(sha(ROOT/r['path'])==r['after'],'queued starter edit preserved '+r['path'])
ok(sha(ROOT/'test/integration/door_first_run_defaults.inc')==queued['new_include_sha256'],'queued starter include preserved')
baseline=ROOT/'build/section11-doors-actions-product-r3';frozen=json.loads((baseline/'source-inputs-before.json').read_text())
for r in frozen:ok(sha(baseline/'source-at-start'/r['path'])==r['sha256'],'accepted frozen input retained '+r['path'])
old=[('Windows-Owner-Test',17866681,'CE56C08B3133AE8E0D31ED5C6000CA73DB4C2689E653F59ECD85CC3F420085D4'),('Engineering-Companion',46143164,'FDFAEBCF86F6B04E8A3BCC6F93B5C9313D041F09F6F6904DEBFE217999BEBD4C')]
for kind,size,h in old:
    p=ROOT/'build'/('SFX-1.0.0-alpha.2-Section11-OwnerFixes-'+kind+'-20261024-r1.zip');ok(p.stat().st_size==size and sha(p)==h,'accepted archive retained '+kind)
source=[]
for p in sorted(COMP.rglob('*')):
    if p.is_file():source.append(dict(path=str(p.relative_to(COMP)).replace('\\','/'),bytes=p.stat().st_size,sha256=sha(p)))
result=dict(result='PASS',checks=len(checks),images=images,source=source,compiler_dependency_closure=closure,upstream_pin=original['commit'],
    scope='Static image/source/build/header closure and accepted input preservation. Direct import floor reviewed; native Win7/Win10 DOS loader and coexistence PENDING.')
(EV/'candidate-inspection.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS',len(checks),'candidate static/source/preservation checks')
