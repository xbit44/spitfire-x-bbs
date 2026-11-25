param([ValidatePattern('^r[0-9]+$')][string]$RunName='r1')
throw 'Historical packet build entry point is disabled in this independent general-file fork. Use build-file.ps1.'
$ErrorActionPreference='Stop'
$packetRepo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if((& git -C $packetRepo rev-parse HEAD) -ne '10773c3729111bba0022e24397d394b7c161d5d0'){throw 'Wrong packet helper worktree'}
$packetSource=Join-Path $PSScriptRoot 'source'
$packetBuild=Join-Path $packetRepo "build/section8d-helper-packet/$RunName"
if(Test-Path -LiteralPath $packetBuild){throw 'Fresh helper identity required; preserve predecessors'}
$packetOriginal=Join-Path $packetRepo 'build/section8b-helper-empty-batch/r4/sexyz.exe'
if((Get-FileHash -LiteralPath $packetOriginal).Hash -ne '5B1FB659D51A0E61B886643219381A173F2BAD3A2BD1570C549BC998E58D34BC'){throw 'Approved text/art helper changed'}
New-Item -ItemType Directory -Path $packetBuild|Out-Null
$packetInputs=@(Get-ChildItem -LiteralPath $PSScriptRoot -File -Recurse|Sort-Object FullName|ForEach-Object{[ordered]@{path=$_.FullName.Substring($PSScriptRoot.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName).Hash;bytes=$_.Length}})
$packetInputs|ConvertTo-Json -Depth 4|Out-File -LiteralPath (Join-Path $packetBuild 'source-inputs-before.json') -Encoding utf8
foreach($packetInput in $packetInputs){$packetCopy=Join-Path $packetBuild "source-at-start/$($packetInput.path)";New-Item -ItemType Directory -Path (Split-Path $packetCopy) -Force|Out-Null;Copy-Item -LiteralPath (Join-Path $PSScriptRoot $packetInput.path) -Destination $packetCopy}
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $packetBuild 'build-packet-at-start.ps1')
$packetVs='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
Import-Module "$packetVs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $packetVs -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64 -vcvars_ver=14.44 -winsdk=10.0.26100.0'
$env:VSLANG='1033'
$packetNames=@('sexyz','xmodem','zmodem','ringbuf','nopen','date_str','telnet','crc16','crc32','datewrap','dirwrap','filewrap','genwrap','ini_file','netwrap','os_info','sockwrap','str_list','threadwrap','xpdatetime','xpevent','xpprintf')
$packetDirs=@('src/sbbs3','src/xpdev','src/hash','src/smblib')|ForEach-Object{Join-Path $packetSource $_}
$packetFlags=@('/nologo','/O2','/MT','/W3','/Brepro','/showIncludes','/DWIN32','/D_CONSOLE','/DSBBS_EXPORTS','/DRINGBUF_EVENT','/DRINGBUF_MUTEX','/DHAS_INTTYPES_H','/DHAS_STDINT_H','/DWINVER=0x601','/D_WIN32_WINNT=0x601','/D_CRT_SECURE_NO_DEPRECATE','/D_CRT_NONSTDC_NO_DEPRECATE','/DNDEBUG',"/I$PSScriptRoot")
$packetFlags+=@($packetDirs|ForEach-Object{"/I$_"})
$packetObjects=@()
Push-Location $packetBuild
try{
    foreach($packetName in $packetNames){
        $packetPath=$packetDirs|ForEach-Object{Join-Path $_ "$packetName.c"}|Where-Object{Test-Path -LiteralPath $_}|Select-Object -First 1
        if(!$packetPath){throw "Missing pinned helper source $packetName"}
        $packetObject=Join-Path $packetBuild "$packetName.obj";$packetObjects+=$packetObject
        & cl.exe @packetFlags /c $packetPath "/Fo$packetObject" 2>&1|Out-File -LiteralPath "$packetName.compile.log" -Encoding utf8
        if($LASTEXITCODE){Get-Content "$packetName.compile.log"|Select-String 'error';throw "Packet helper compile failed: $packetName"}
    }
    & link.exe /nologo /Brepro /INCREMENTAL:NO /SUBSYSTEM:CONSOLE,6.01 /MACHINE:X86 /OUT:SFXQWKTransfer.exe @packetObjects ws2_32.lib iphlpapi.lib 2>&1|Tee-Object -FilePath link.log
    if($LASTEXITCODE){throw 'Packet helper link failed'}
    & ./SFXQWKTransfer.exe --version 2>&1|Tee-Object -FilePath version.log
    if($LASTEXITCODE){throw 'Packet helper version probe failed'}
    & dumpbin.exe /headers /imports ./SFXQWKTransfer.exe|Out-File -LiteralPath pe-imports.log -Encoding utf8
    foreach($packetInput in $packetInputs){if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $packetInput.path)).Hash -ne $packetInput.sha256){throw 'Packet helper source changed during bound build'}}
    if((Get-FileHash -LiteralPath $packetOriginal).Hash -ne '5B1FB659D51A0E61B886643219381A173F2BAD3A2BD1570C549BC998E58D34BC'){throw 'Approved text/art helper changed during build'}
    [ordered]@{status='BUILD ONLY; NOT protocol/Runtime/owner qualification';upstream='d83fbe2a41580e37314ea671bb54fd9fedb338cd';receive='SFX-PACKET-RECEIVE 1';send='SFX-PACKET-SEND 1';binary='SFXQWKTransfer.exe';sha256=(Get-FileHash SFXQWKTransfer.exe).Hash;inputs=$packetInputs}|ConvertTo-Json -Depth 6|Out-File -LiteralPath manifest.json -Encoding utf8
    Get-FileHash SFXQWKTransfer.exe
}finally{Pop-Location}
