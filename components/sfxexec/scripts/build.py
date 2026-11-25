"""Build only the external GPL VDD and matching DOS launcher; no install."""
from pathlib import Path
import subprocess, os, json, hashlib, sys
ROOT=Path(__file__).resolve().parents[3];COMP=ROOT/'components/sfxexec'
EV=ROOT/'research/architecture/evidence/section11-sfxexec-components-20261024'
TOOLS=ROOT/'build/sfxexec-tools-20261024/MSVC15'
BUILD=ROOT/'build'/('sfxexec-components-'+(sys.argv[1] if len(sys.argv)>1 else 'r1'))
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest().upper()
def run(args,cwd,env,log):
    result=subprocess.run([str(x) for x in args],cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    Path(log).write_bytes(result.stdout)
    if result.returncode:raise RuntimeError(f'exit {result.returncode}: {log}\n'+result.stdout.decode('mbcs',errors='replace')[-5500:])
    return result.stdout
def main():
    assert not BUILD.exists(),'fresh component build required'
    BUILD.mkdir();(BUILD/'dll').mkdir();(BUILD/'launcher').mkdir()
    envbat=BUILD/'compiler-env.bat'
    envbat.write_text('@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat" >nul\nif errorlevel 1 exit /b 1\nset\n')
    envtext=subprocess.check_output(['cmd.exe','/d','/c',str(envbat)],text=True,encoding='mbcs')
    modern={k.upper():v for k,v in os.environ.items()}
    for line in envtext.splitlines():
        if '=' in line and not line.startswith('='):
            key,value=line.split('=',1);modern[key.upper()]=value
    modernbin=Path(modern['VCTOOLSINSTALLDIR'])/'bin/Hostx64/x86'
    assert (modernbin/'cl.exe').exists()
    commands=[]
    units=json.loads((COMP/'UPSTREAM.json').read_text())['files']
    units=[r['path'] for r in units if r['path'].endswith('.c') and '/dosxtrn/' not in r['path']]
    flags=['/nologo','/c','/MT','/O2','/W3','/std:c11','/Gz','/arch:IA32','/Brepro','/showIncludes',
        '/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/Di386','/DWIN32','/DRINGBUF_EVENT','/DRINGBUF_MUTEX',
        '/DNO_SOCKET_SUPPORT','/DHAS_INTTYPES_H','/DHAS_STDINT_H','/DXPDEV_THREAD_SAFE','/DLINK_LIST_THREADSAFE',
        '/D_CRT_SECURE_NO_WARNINGS','/D_CRT_NONSTDC_NO_DEPRECATE',
        '/I'+str(COMP/'src/src/sbbs3'),'/I'+str(COMP/'src/src/xpdev')]
    objects=[]
    for unit in units:
        obj=BUILD/'dll'/(Path(unit).stem+'.obj');args=[modernbin/'cl.exe',*flags,'/Fo'+str(obj),str(COMP/'src'/unit)]
        commands.append(args);run(args,BUILD,modern,BUILD/'dll'/(Path(unit).stem+'.log'));objects.append(obj)
    dll=BUILD/'SFXEXEC.DLL'
    args=[modernbin/'link.exe','/NOLOGO','/DLL','/MACHINE:X86','/SUBSYSTEM:WINDOWS,6.01','/DYNAMICBASE','/NXCOMPAT',
        '/Brepro','/INCREMENTAL:NO','/OPT:REF','/OUT:'+str(dll),*objects,
        'ntvdm.lib','netapi32.lib','kernel32.lib','user32.lib','advapi32.lib','ws2_32.lib','iphlpapi.lib','ole32.lib','uuid.lib']
    commands.append([str(x) for x in args]);run(args,BUILD,modern,BUILD/'link-vdd.log')
    print('PASS built Win32/x86 SFXEXEC.DLL',flush=True)
    if '--dll-only' in sys.argv:
        run([modernbin/'dumpbin.exe','/HEADERS','/IMPORTS','/EXPORTS',str(dll)],BUILD,modern,BUILD/'SFXEXEC-pe.txt')
        (BUILD/'build.json').write_text(json.dumps(dict(result='PASS DLL compilation only; qualification pending',
            build=str(BUILD),commands=[[str(x) for x in command] for command in commands],
            source=[dict(path=str(p.relative_to(COMP)).replace('\\','/'),sha256=sha(p)) for p in sorted((COMP/'src').rglob('*')) if p.is_file()],
            outputs=[dict(path=str(dll),bytes=dll.stat().st_size,sha256=sha(dll))],
            tools=dict(modern_vc=modern.get('VCTOOLSINSTALLDIR'),sdk=modern.get('WINDOWSSDKVERSION'))),indent=2)+'\n')
        return
    legacy={k.upper():v for k,v in os.environ.items()};legacy['PATH']=str(TOOLS/'BIN')+';'+legacy['PATH'];legacy['INCLUDE']=str(TOOLS/'INCLUDE');legacy['LIB']=str(TOOLS/'LIB')
    launcher=BUILD/'launcher';source=(COMP/'src/src/sbbs3/dosxtrn/dosxtrn.c').read_bytes();(launcher/'SFXDOOR.C').write_bytes(source)
    (launcher/'SFXDOOR.DEF').write_text('EXETYPE DOS\n')
    args=[TOOLS/'BIN/CL.EXE','/AS','/Gs','/Fc','/Fm','/FeSFXDOOR.EXE','/I'+str(COMP/'src/src/sbbs3'),
        '/I'+str(ROOT/'build/sfxexec-tools-20261024/DDK'),'SFXDOOR.C']
    commands.append([str(x) for x in args]);run(args,launcher,legacy,BUILD/'legacy-launcher.log')
    exe=launcher/'SFXDOOR.EXE';assert exe.exists()
    print('PASS built matching 16-bit DOS SFXDOOR.EXE',flush=True)
    run([modernbin/'dumpbin.exe','/HEADERS','/IMPORTS','/EXPORTS',str(dll)],BUILD,modern,BUILD/'SFXEXEC-pe.txt')
    inputs=[dict(path=str(p.relative_to(COMP)).replace('\\','/'),sha256=sha(p)) for p in sorted((COMP/'src').rglob('*')) if p.is_file()]
    result=dict(result='PASS component compilation only; qualification pending',build=str(BUILD),commands=[[str(x) for x in command] for command in commands],
        source=inputs,outputs=[dict(path=str(p),bytes=p.stat().st_size,sha256=sha(p)) for p in (dll,exe)],
        tools=dict(modern_vc=modern.get('VCTOOLSINSTALLDIR'),sdk=modern.get('WINDOWSSDKVERSION'),legacy_cl_sha256=sha(TOOLS/'BIN/CL.EXE')),
        no_install=True,no_setup=True,no_owner_access=True)
    (BUILD/'build.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
