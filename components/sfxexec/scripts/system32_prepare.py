"""Preserve r4 for the NEW owner System32 GO, which explicitly supersedes R251.
Direct human approval: DR4 turn01a17057-efee-74b1-806c-40d58779b37f,
message01a17057-f053-7981-94e0-cbe6beac081e, read independently by CR4.
This script has no system-directory, owner-board or production operations.
"""
from pathlib import Path
import hashlib,json,shutil,subprocess
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
EV=ROOT/'research/architecture/evidence/section11-sfxexec-system32-20261024'
assert not EV.exists();EV.mkdir()
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
handoff=ROOT/'project-records/design/2026-10-24/sfx-dos-door-components/CR4_SFXEXEC_SYSTEM32_HANDOFF_APPROVED_20261024.txt'
assert sha(handoff)=='8110682F0BF66171362EC50CA45AD18F792F27F28E60C10B4019571614215097'
shutil.copy2(handoff,EV/handoff.name)
prior=ROOT/'research/architecture/evidence/section11-sfxexec-loader-fix-20261024'
record=json.loads((prior/'inspection-r3.json').read_text());files=[]
for r in record['files']:
    p=COMP/r['path'];assert sha(p)==r['sha256'],r['path']
    q=EV/'baseline-r4'/r['path'];q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q);files.append(r)
archives=[]
for r in json.loads((prior/'delivery-r4.json').read_text()).values():
    if not isinstance(r,dict) or 'archive' not in r:continue
    p=Path(r['archive']);assert p.stat().st_size==r['bytes'] and sha(p)==r['sha256'];archives.append(r)
for name in ('AGENTS.md','PROJECT_STATUS.md','DECISIONS.md'):
    shutil.copy2(ROOT/name,EV/name)
review=ROOT/'project-records/design/2026-10-24/sfx-dos-door-components/loader-fix-review/r4-native-retry'
for r in json.loads((review/'EVIDENCE_INDEX_20261024.json').read_text())['files']:
    assert sha(review/r['name'])==r['sha256']
(EV/'git-status-before.txt').write_bytes(subprocess.check_output(['git','status','--porcelain=v1'],cwd=ROOT))
(EV/'baseline-r4.json').write_text(json.dumps(dict(files=files,archives=archives,handoff_sha256=sha(handoff),owner_thread='01a1608c-bf89-7e83-b9b0-7902b053c032',owner_turn='01a17057-efee-74b1-806c-40d58779b37f',owner_message='01a17057-f053-7981-94e0-cbe6beac081e',instruction='approved - GO. send the updated System32 handoff to CR-4 and handle any Q&A if needed.'),indent=2)+'\n')
print('Preserved',len(files),'exact r4 component files; both r4 ZIPs and native evidence verified; human GO independently read.')
