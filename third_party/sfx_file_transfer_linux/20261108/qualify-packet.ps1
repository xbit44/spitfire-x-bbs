param([ValidatePattern('^r[0-9]+$')][string]$RunName='r1',[ValidatePattern('^r[0-9]+$')][string]$HelperRun='r4',[switch]$FrozenDiagnostic)
$ErrorActionPreference='Stop'
$packetRepo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$packetHelperBuild=Join-Path $packetRepo "build/section8d-helper-packet/$HelperRun"
$packetTestBuild=Join-Path $packetRepo "build/section8d-helper-packet-tests/$RunName"
if(Test-Path -LiteralPath $packetTestBuild){throw 'Fresh packet qualification identity required'}
$packetManifest=Get-Content -LiteralPath (Join-Path $packetHelperBuild 'manifest.json') -Raw|ConvertFrom-Json
$packetValidationSource=$PSScriptRoot
if($FrozenDiagnostic){if($HelperRun -ne 'r12' -or $packetManifest.sha256 -ne '24EE8D93FE5A70731C0BB5444262BE9B838E4CC369054DDE6323BBB15E678EB1'){throw 'Only exact frozen prior helper diagnostic permitted'};$packetValidationSource=Join-Path $packetHelperBuild 'source-at-start'}
foreach($packetInput in $packetManifest.inputs){if((Get-FileHash -LiteralPath (Join-Path $packetValidationSource $packetInput.path)).Hash -ne $packetInput.sha256){throw "Candidate input drift: $($packetInput.path)"}}
if((Get-FileHash -LiteralPath (Join-Path $packetHelperBuild 'SFXQWKTransfer.exe')).Hash -ne $packetManifest.sha256){throw 'Candidate binary drift'}
New-Item -ItemType Directory -Path $packetTestBuild|Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'packet-protocol-tests.cpp') -Destination (Join-Path $packetTestBuild 'packet-protocol-tests-at-start.cpp')
Copy-Item -LiteralPath (Join-Path $packetHelperBuild 'manifest.json') -Destination (Join-Path $packetTestBuild 'candidate-manifest.json')
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $packetTestBuild 'qualify-packet-at-start.ps1')
$packetTestHash=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'packet-protocol-tests.cpp')).Hash
$packetVs='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
Import-Module "$packetVs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $packetVs -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64 -vcvars_ver=14.44 -winsdk=10.0.26100.0'
$env:VSLANG='1033'
$packetFlags=@('/nologo','/std:c++17','/EHsc','/O2','/MT','/W4','/WX','/Brepro','/DWIN32','/DHAS_STDINT_H','/DHAS_INTTYPES_H','/D_WIN32_WINNT=0x601','/D_CRT_SECURE_NO_WARNINGS')
$packetFlags+=@('src/sbbs3','src/xpdev','src/hash','src/smblib')|ForEach-Object{"/I$(Join-Path (Join-Path $PSScriptRoot 'source') $_)"}
$packetObjects=@(Get-ChildItem -LiteralPath $packetHelperBuild -Filter '*.obj' -File|Where-Object{$_.Name -ne 'sexyz.obj'}|ForEach-Object{$_.FullName})
Push-Location $packetTestBuild
try{
    & cl.exe @packetFlags (Join-Path $PSScriptRoot 'packet-protocol-tests.cpp') @packetObjects /Fe:packet-protocol-tests.exe /link ws2_32.lib iphlpapi.lib /SUBSYSTEM:CONSOLE,6.01 2>&1|Tee-Object -FilePath build.log
    if($LASTEXITCODE){throw 'Packet peer compilation failed; keep evidence'}
    & ./packet-protocol-tests.exe (Join-Path $packetHelperBuild 'SFXQWKTransfer.exe') (Join-Path $packetTestBuild 'fixtures') 2>&1|Tee-Object -FilePath protocol-tests.log
    if($LASTEXITCODE){throw 'Packet actual-process qualification failed; keep evidence'}
    if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'packet-protocol-tests.cpp')).Hash -ne $packetTestHash){throw 'Test source changed during qualification'}
    foreach($packetInput in $packetManifest.inputs){if((Get-FileHash -LiteralPath (Join-Path $packetValidationSource $packetInput.path)).Hash -ne $packetInput.sha256){throw 'Candidate input changed during qualification'}}
    if($FrozenDiagnostic){throw 'Frozen diagnostic only; never create current helper qualification clearance'}
    [ordered]@{status='PASS exact tested private helper process subset; NOT Runtime/Telnet/SSH/NCM/owner pass';helper_sha256=$packetManifest.sha256;test_source_sha256=$packetTestHash;test_binary_sha256=(Get-FileHash packet-protocol-tests.exe).Hash;log_sha256=(Get-FileHash protocol-tests.log).Hash}|ConvertTo-Json -Depth 4|Out-File -LiteralPath qualification.json -Encoding utf8
}finally{Pop-Location}
