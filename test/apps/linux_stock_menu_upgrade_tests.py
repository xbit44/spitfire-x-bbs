"""Exact predecessor-pair migration and preservation through the real installer."""
from pathlib import Path
import hashlib, importlib.util, json, os, sys

source, case = Path(sys.argv[1]), Path(sys.argv[2])
assert sys.platform == 'linux' and os.geteuid() != 0 and not case.exists()
case.mkdir(mode=0o700)
spec=importlib.util.spec_from_file_location('installer',source/'distribution/linux/install.py')
installer=importlib.util.module_from_spec(spec);spec.loader.exec_module(installer)
package=case/'package';package.mkdir(mode=0o700)
new={ext:(source/('distribution/Display/main.'+ext)).read_bytes() for ext in ('ans','asc')}
old={ext:(source/('test/fixtures/rc9-stock/main.'+ext)).read_bytes() for ext in ('ans','asc')}
files={'Bin/SFXWeb':b'program','Spitfire-X':b'launcher',**{'Display/main.'+ext:data for ext,data in new.items()}}
rows=[]
for name,data in files.items():
    p=package/'payload'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data);p.chmod(0o644)
    rows.append(dict(path=name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),policy='fresh' if name.startswith('Display/') else 'always',mode='0644' if name.startswith('Display/') else '0755'))
(package/'package-manifest.json').write_text(json.dumps(dict(format=1,version='rc10-test',files=rows)))
(package/'package-manifest.json').chmod(0o644)
root=case/'board';installer.install(package,root)
paths={ext:root/('Display/main.'+ext) for ext in new}
checks=[]
def check(value,label):
    assert value,label
    checks.append(label)
def predecessor():
    for ext,path in paths.items():path.write_bytes(old[ext])
check(all(p.read_bytes()==new[e] for e,p in paths.items()),'fresh installation has matching corrected pair')
predecessor();installer.install(package,root,True)
check(all(p.read_bytes()==new[e] for e,p in paths.items()),'exact old ANSI and plain pair update together')
predecessor();paths['asc'].write_bytes(b'owner custom ASC');installer.install(package,root,True)
check(paths['ans'].read_bytes()==old['ans'] and paths['asc'].read_bytes()==b'owner custom ASC','one custom member preserves both files')
paths['asc'].unlink();installer.install(package,root,True)
check(paths['ans'].read_bytes()==old['ans'] and not paths['asc'].exists(),'missing member preserves mixed pair without synthesizing its companion')
predecessor();real_replace=installer.os.replace;failed=False
def interrupt(src,dst):
    global failed
    if not failed and '/new/Display/' in src and src.endswith('main.asc'):
        failed=True
        raise OSError('pair publication interrupted')
    return real_replace(src,dst)
installer.os.replace=interrupt
try:
    installer.install(package,root,True)
    raise AssertionError('pair fault not exercised')
except OSError as error:
    check('pair publication interrupted' in str(error),'second member publication failure surfaced')
finally:installer.os.replace=real_replace
check(all(p.read_bytes()==old[e] for e,p in paths.items()),'interrupted pair update rolls both files back exactly')
installer.install(package,root,True);stamps={e:p.stat().st_mtime_ns for e,p in paths.items()};installer.install(package,root,True)
check(all(p.read_bytes()==new[e] and p.stat().st_mtime_ns==stamps[e] for e,p in paths.items()),'already-current pair is not rewritten')
(case/'checks.json').write_text(json.dumps(checks,indent=2))
print('PASS',len(checks),'stock menu upgrade checks')
