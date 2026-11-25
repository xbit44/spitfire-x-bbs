param([ValidatePattern('^r[0-9]+$')][string]$RunName='r1')
$ErrorActionPreference='Stop'
$fileRepo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if((& git -C $fileRepo rev-parse HEAD) -ne '10773c3729111bba0022e24397d394b7c161d5d0' -or (& git -C $fileRepo branch --show-current) -ne 'main'){throw 'Wrong canonical checkout'}
$fileSource=Join-Path $PSScriptRoot 'source'
$fileBuild=Join-Path $fileRepo "build/section10-helper-file/$RunName"
if(Test-Path -LiteralPath $fileBuild){throw 'Fresh helper build required'}
$fileFork=Get-Content -LiteralPath (Join-Path $fileRepo 'research/architecture/evidence/section9-20261017/file-helper-fork-r1.json') -Raw|ConvertFrom-Json
$fileFork.source=Join-Path $fileRepo 'research/architecture/section8d-packet-helper'
foreach($entry in $fileFork.inputs){if((Get-FileHash -LiteralPath (Join-Path $fileFork.source $entry.path)).Hash -ne $entry.sha256){throw 'Accepted packet source changed'}}
foreach($entry in $fileFork.accepted_images){if((Get-FileHash -LiteralPath (Join-Path $fileRepo $entry.path)).Hash -ne $entry.sha256){throw 'Accepted helper image changed'}}
New-Item -ItemType Directory -Path $fileBuild|Out-Null
$fileInputs=@(Get-ChildItem -LiteralPath $PSScriptRoot -File -Recurse|Sort-Object FullName|ForEach-Object{[ordered]@{path=$_.FullName.Substring($PSScriptRoot.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName).Hash;bytes=$_.Length}})
$fileInputs|ConvertTo-Json -Depth 4|Out-File -LiteralPath (Join-Path $fileBuild 'source-inputs-before.json') -Encoding utf8
foreach($entry in $fileInputs){$destination=Join-Path $fileBuild "source-at-start/$($entry.path)";New-Item -ItemType Directory -Path (Split-Path $destination) -Force|Out-Null;Copy-Item -LiteralPath (Join-Path $PSScriptRoot $entry.path) -Destination $destination}
$fileVs='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
Import-Module "$fileVs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $fileVs -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64 -vcvars_ver=14.44 -winsdk=10.0.26100.0'
$env:VSLANG='1033'
$fileNames=@('sexyz','xmodem','zmodem','ringbuf','nopen','date_str','telnet','crc16','crc32','datewrap','dirwrap','filewrap','genwrap','ini_file','netwrap','os_info','sockwrap','str_list','threadwrap','xpdatetime','xpevent','xpprintf')
$fileDirs=@('src/sbbs3','src/xpdev','src/hash','src/smblib')|ForEach-Object{Join-Path $fileSource $_}
$fileFlags=@('/nologo','/O2','/MT','/W3','/Brepro','/showIncludes','/DWIN32','/D_CONSOLE','/DSBBS_EXPORTS','/DRINGBUF_EVENT','/DRINGBUF_MUTEX','/DHAS_INTTYPES_H','/DHAS_STDINT_H','/DWINVER=0x601','/D_WIN32_WINNT=0x601','/D_CRT_SECURE_NO_DEPRECATE','/D_CRT_NONSTDC_NO_DEPRECATE','/DNDEBUG',"/I$PSScriptRoot")
$fileFlags+=@($fileDirs|ForEach-Object{"/I$_"});$fileObjects=@()
Push-Location $fileBuild
try{
    foreach($name in $fileNames){
        $path=$fileDirs|ForEach-Object{Join-Path $_ "$name.c"}|Where-Object{Test-Path -LiteralPath $_}|Select-Object -First 1
        if(!$path){throw "Pinned helper source missing: $name"}
        $object=Join-Path $fileBuild "$name.obj";$fileObjects+=$object
        & cl.exe @fileFlags /c $path "/Fo$object" 2>&1|Out-File -LiteralPath "$name.compile.log" -Encoding utf8
        if($LASTEXITCODE){Get-Content "$name.compile.log"|Select-String 'error';throw "Helper compile failed: $name"}
    }
    & link.exe /nologo /Brepro /INCREMENTAL:NO /SUBSYSTEM:CONSOLE,6.01 /MACHINE:X86 /OUT:SFXFileTransfer.exe @fileObjects ws2_32.lib iphlpapi.lib 2>&1|Tee-Object -FilePath link.log
    if($LASTEXITCODE){throw 'Helper link failed'}
    & ./SFXFileTransfer.exe --version 2>&1|Tee-Object -FilePath version.log
    if($LASTEXITCODE){throw 'Helper version probe failed'}
    & dumpbin.exe /headers /imports ./SFXFileTransfer.exe|Out-File -LiteralPath pe-imports.log -Encoding utf8
    foreach($entry in $fileInputs){if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $entry.path)).Hash -ne $entry.sha256){throw 'Helper inputs changed during build'}}
    foreach($entry in $fileFork.inputs){if((Get-FileHash -LiteralPath (Join-Path $fileFork.source $entry.path)).Hash -ne $entry.sha256){throw 'Accepted packet source changed during build'}}
    foreach($entry in $fileFork.accepted_images){if((Get-FileHash -LiteralPath (Join-Path $fileRepo $entry.path)).Hash -ne $entry.sha256){throw 'Accepted helper image changed during build'}}
    [ordered]@{status='BUILD ONLY; NOT protocol/host/installed qualification';upstream=$fileFork.upstream;profile='SFX-FILE-TRANSFER 1 Z; 2 X Y G';binary='SFXFileTransfer.exe';sha256=(Get-FileHash SFXFileTransfer.exe).Hash;inputs=$fileInputs;accepted_images=$fileFork.accepted_images}|ConvertTo-Json -Depth 7|Out-File -LiteralPath manifest.json -Encoding utf8
    Get-FileHash SFXFileTransfer.exe
}finally{Pop-Location}
