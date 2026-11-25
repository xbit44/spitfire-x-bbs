"""Verify retained provenance and prepare private SFX VDD/legacy-tool inputs."""
from pathlib import Path
import hashlib, json, shutil, urllib.request

ROOT=Path(__file__).resolve().parents[3]
COMP=ROOT/'components/sfxexec'
EV=ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024'
TOOLS=ROOT/'build/sfxexec-tools-20261024'
MEDIA=ROOT/'research/architecture/evidence/section11-dos-toolchain-20261024'
REF=ROOT/'research/architecture/evidence/section11-door-owner-inputs-20261022/sbbsexec-build-feasibility'
PIN='97fab8d84a68882d1ceff0bed7cc05497f4d5973'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
def load(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,v):p=Path(p);p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(v,indent=2)+'\n')

def main():
    assert not EV.exists() and not TOOLS.exists(),'fresh evidence/tools required'
    EV.mkdir(parents=True);TOOLS.mkdir(parents=True)
    handoff=ROOT/'project-records/design/2026-10-24/sfx-dos-door-components/CR4_SFX_DOS_DLL_LAUNCHER_HANDOFF_APPROVED_20261024.txt'
    assert sha(handoff)=='2FBC2FCEF2E14B2EA7F8839E49B89B6B5A8799F1FA3FFCD34E02515ACD6C5AAC'
    shutil.copy2(handoff,EV/handoff.name)
    frozen=ROOT/'build/section11-doors-actions-product-r3'
    changed=[]
    for row in load(frozen/'source-inputs-before.json'):
        assert sha(frozen/'source-at-start'/row['path'])==row['sha256']
        actual=sha(ROOT/row['path'])
        if actual!=row['sha256']:changed.append(dict(path=row['path'],before=row['sha256'],after=actual))
    assert {r['path'].replace('\\','/') for r in changed}=={
        'src/adapters/storage/src/sqlite_account_repository.cpp','test/integration/door_storage_tests.cpp'}
    write(EV/'queued-category-source.json',dict(excluded_from_component_package=True,changes=changed,
        new_include_sha256=sha(ROOT/'test/integration/door_first_run_defaults.inc')))
    provenance=load(MEDIA/'media-provenance.json');verified=[]
    for row in provenance['artifact_hashes']:
        p=MEDIA/row['file'];assert p.stat().st_size==row['bytes'] and sha(p)==row['sha256'],row['file']
        verified.append(row)
    c=provenance['compiler'];assert hashlib.sha512((MEDIA/c['archive']).read_bytes()).hexdigest().upper()==c['archive_sha512']
    write(EV/'tool-provenance-verified.json',dict(result='PASS',media=verified,compiler_sha512=c['archive_sha512'],
        scope='Retained exact media; no installer/global PATH/production access'))
    entries=load(MEDIA/'msvc152c-iso-index.json')['entries'];extracted=[]
    with (MEDIA/c['iso']).open('rb') as iso:
        for row in entries:
            name=row['path']
            if row['directory'] or not (name.startswith('MSVC15/BIN/') or name.startswith('MSVC15/INCLUDE/') or name in {'MSVC15/LIB/SLIBCE.LIB','MSVC15/LIB/OLDNAMES.LIB'}):continue
            assert '..' not in Path(name).parts
            iso.seek(row['lba']*2048);data=iso.read(row['size']);assert len(data)==row['size']
            out=TOOLS/name;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data)
            extracted.append(dict(path=name,bytes=len(data),sha256=sha(out)))
    assert (TOOLS/'MSVC15/LIB/SLIBCE.LIB').exists()
    write(EV/'private-tool-extraction.json',dict(iso_sha256=c['iso_sha256'],files=extracted))
    review=load(REF/'component-source-review.json');assert review['upstream_commit']==PIN
    sources=[]
    for row in review['reference_files']:
        source=Path(row['stored']);assert sha(source)==row['sha256'],row['path']
        for tree in ('upstream','src'):
            target=COMP/tree/row['path'];target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        sources.append(dict(path=row['path'],sha256=row['sha256'],url=row['url']))
    for row in load(REF/'source-manifest.json')['files']:
        source=Path(row['stored']);assert sha(source)==row['sha256']
        for tree in ('upstream','src'):
            target=COMP/tree/row['source_path'];target.parent.mkdir(parents=True,exist_ok=True)
            if not target.exists():shutil.copy2(source,target)
    write(COMP/'UPSTREAM.json',dict(repository='https://github.com/SynchronetBBS/sbbs',commit=PIN,files=sources))
    # Proprietary headers/tools stay in private build inputs, outside source ZIP.
    (TOOLS/'DDK').mkdir()
    shutil.copy2(MEDIA/'ddk-vdd-headers/wnet/isvbop.h',TOOLS/'DDK/isvbop.h')
    print('PASS verified media/source pin; private legacy tool extraction; original source notices preserved')
if __name__=='__main__':main()
