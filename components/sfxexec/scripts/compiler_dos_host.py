"""Private build-only DOS host for the verified Microsoft compiler, never shipped."""
from pathlib import Path
import urllib.request,hashlib,zipfile,json,subprocess,os,sys
ROOT=Path(__file__).resolve().parents[3];BASE=ROOT/'build/sfxexec-tools-20261024'
DEST=BASE/'compiler-dos-host';ARCHIVE=BASE/'dosbox-x-mingw64-2026.10.01-portable.zip'
URL='https://github.com/joncampbell123/dosbox-x/releases/download/dosbox-x-v2026.10.01/dosbox-x-mingw64-2026.10.01-portable.zip'
SHA='DEF2D84D4415FC26C77252839B4DAFB6B476E3A4204B896292E6B05505CA8C59'
if not ARCHIVE.exists():
    with urllib.request.urlopen(URL,timeout=60) as response,ARCHIVE.open('wb') as output:
        while chunk:=response.read(1024*1024):output.write(chunk)
assert hashlib.sha256(ARCHIVE.read_bytes()).hexdigest().upper()==SHA
if not DEST.exists():
    with zipfile.ZipFile(ARCHIVE) as archive:
        for entry in archive.infolist():
            path=Path(entry.filename);assert not path.is_absolute() and '..' not in path.parts
        archive.extractall(DEST)
exes=list(DEST.rglob('dosbox-x.exe'));assert exes
exe=next((p for p in exes if 'sdl2' in str(p).lower()),exes[0])
record=dict(url=URL,sha256=SHA,exe=str(exe),exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest().upper(),
    role='Private compiler execution only. Not an SFX runtime dependency, no DOS/VDD coexistence qualification, no installer/global path/owner changes.')
(ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024/compiler-host.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record),flush=True)
