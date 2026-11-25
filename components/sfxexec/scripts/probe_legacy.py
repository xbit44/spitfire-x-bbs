"""Bounded compiler-host diagnosis; no tool patch or machine setting change."""
from pathlib import Path
import os,subprocess,json
ROOT=Path(__file__).resolve().parents[3];TOOLS=ROOT/'build/sfxexec-tools-20261024/MSVC15'
OUT=ROOT/'build/sfxexec-legacy-probes';OUT.mkdir(exist_ok=False)
(OUT/'tiny.c').write_text('int main(void) { return 0; }\n')
base={k.upper():v for k,v in os.environ.items()}
small={k:base[k] for k in ['SYSTEMROOT','WINDIR','TEMP','TMP','COMSPEC'] if k in base}
small.update(PATH=str(TOOLS/'BIN')+';'+base['SYSTEMROOT']+'\\System32',INCLUDE=str(TOOLS/'INCLUDE'),LIB=str(TOOLS/'LIB'))
results=[]
for name,extra,flags in [('minimal',{},[]),('xp',{'__COMPAT_LAYER':'WinXPSP3'},[]),
    ('explicit-passes',{},['/B1'+str(TOOLS/'BIN/C13216.EXE'),'/B2'+str(TOOLS/'BIN/C23216.EXE')])]:
    env={**small,**extra};args=[str(TOOLS/'BIN/CL.EXE'),'/c','/AS','/Fotiny-'+name+'.obj',*flags,'tiny.c']
    try:
        p=subprocess.run(args,cwd=OUT,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
        (OUT/(name+'.log')).write_bytes(p.stdout);results.append(dict(case=name,exit=p.returncode,output=p.stdout.decode('mbcs',errors='replace')))
    except Exception as e:results.append(dict(case=name,error=str(e)))
    print(json.dumps(results[-1]),flush=True)
(OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
