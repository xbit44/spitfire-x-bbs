param([ValidatePattern('^r[0-9]+$')][string]$RunName='r1',[ValidatePattern('^r[0-9]+$')][string]$HelperRun='r2')
$ErrorActionPreference='Stop'
$fileRepo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$fileBuild=Join-Path $fileRepo "build/section9-helper-file/$HelperRun"
$fileTest=Join-Path $fileRepo "build/section9-helper-file-tests/$RunName"
$fileRuns='C:\sfxtst\runs'
foreach($path in @('C:\sfxtst',$fileRuns)){if(!(Test-Path -LiteralPath $path -PathType Container) -or ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Unsafe fixture parent'}}
$fileFixture=Join-Path $fileRuns "section9-file-helper-$RunName"
if((Test-Path -LiteralPath $fileTest) -or (Test-Path -LiteralPath $fileFixture)){throw 'Fresh qualification identity required'}
$fileManifest=Get-Content -LiteralPath (Join-Path $fileBuild 'manifest.json') -Raw|ConvertFrom-Json
foreach($entry in $fileManifest.inputs){if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $entry.path)).Hash -ne $entry.sha256){throw "Candidate drift: $($entry.path)"}}
if((Get-FileHash -LiteralPath (Join-Path $fileBuild 'SFXFileTransfer.exe')).Hash -ne $fileManifest.sha256){throw 'Candidate binary drift'}
New-Item -ItemType Directory -Path $fileTest|Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'file-protocol-tests.cpp') -Destination (Join-Path $fileTest 'file-protocol-tests-at-start.cpp')
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $fileTest 'qualify-file-at-start.ps1')
Copy-Item -LiteralPath (Join-Path $fileBuild 'manifest.json') -Destination (Join-Path $fileTest 'candidate-manifest.json')
$fileVs='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
Import-Module "$fileVs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $fileVs -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64 -vcvars_ver=14.44 -winsdk=10.0.26100.0'
$env:VSLANG='1033'
$flags=@('/nologo','/std:c++17','/EHsc','/O2','/MT','/W4','/WX','/Brepro','/DWIN32','/DHAS_STDINT_H','/DHAS_INTTYPES_H','/D_WIN32_WINNT=0x601','/D_CRT_SECURE_NO_WARNINGS')
$flags+=@('src/sbbs3','src/xpdev','src/hash','src/smblib')|ForEach-Object{"/I$(Join-Path (Join-Path $PSScriptRoot 'source') $_)"}
$objects=@(Get-ChildItem -LiteralPath $fileBuild -Filter '*.obj' -File|Where-Object{$_.Name -ne 'sexyz.obj'}|ForEach-Object{$_.FullName})
Push-Location $fileTest
try{
    & cl.exe @flags (Join-Path $PSScriptRoot 'file-protocol-tests.cpp') @objects /Fe:file-protocol-tests.exe /link ws2_32.lib iphlpapi.lib /SUBSYSTEM:CONSOLE,6.01 2>&1|Tee-Object -FilePath build.log
    if($LASTEXITCODE){throw 'File peer compilation failed; retain evidence'}
    & ./file-protocol-tests.exe (Join-Path $fileBuild 'SFXFileTransfer.exe') $fileFixture 2>&1|Tee-Object -FilePath protocol-tests.log
    if($LASTEXITCODE){throw 'Actual-process qualification failed; retain evidence'}
    foreach($entry in $fileManifest.inputs){if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $entry.path)).Hash -ne $entry.sha256){throw 'Candidate changed during qualification'}}
    [ordered]@{status='PASS actual separate file-helper process subset; NOT installed Runtime/Telnet/SSH';helper_sha256=$fileManifest.sha256;fixture=$fileFixture;test_sha256=(Get-FileHash file-protocol-tests.exe).Hash;log_sha256=(Get-FileHash protocol-tests.log).Hash}|ConvertTo-Json -Depth 4|Out-File -LiteralPath qualification.json -Encoding utf8
}finally{Pop-Location}
