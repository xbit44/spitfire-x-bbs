param([ValidatePattern('^r[0-9]+$')][string]$RunName='r1',[switch]$BuildOnly)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$source=Join-Path $PSScriptRoot 'source'
$build=Join-Path $repo "build/section8b-helper-empty-batch/$RunName"
if(Test-Path -LiteralPath $build){throw 'Run exists; use new identity, retain previous evidence'}
New-Item -ItemType Directory -Path $build | Out-Null
$vs='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64 -vcvars_ver=14.44 -winsdk=10.0.26100.0'
$env:VSLANG='1033'
$names=@('sexyz','xmodem','zmodem','ringbuf','nopen','date_str','telnet','crc16','crc32','datewrap','dirwrap','filewrap','genwrap','ini_file','netwrap','os_info','sockwrap','str_list','threadwrap','xpdatetime','xpevent','xpprintf')
$dirs=@('src/sbbs3','src/xpdev','src/hash','src/smblib') | ForEach-Object {Join-Path $source $_}
$flags=@('/nologo','/O2','/MT','/W3','/Brepro','/showIncludes','/DWIN32','/D_CONSOLE','/DSBBS_EXPORTS','/DRINGBUF_EVENT','/DRINGBUF_MUTEX','/DHAS_INTTYPES_H','/DHAS_STDINT_H','/DWINVER=0x601','/D_WIN32_WINNT=0x601','/D_CRT_SECURE_NO_DEPRECATE','/D_CRT_NONSTDC_NO_DEPRECATE','/DNDEBUG',"/I$PSScriptRoot")
$flags+= $dirs | ForEach-Object {"/I$_"}
$objects=@()
Push-Location $build
try {
  foreach($name in $names){
    $path=$dirs | ForEach-Object {Join-Path $_ "$name.c"} | Where-Object {Test-Path -LiteralPath $_} | Select-Object -First 1
    if(!$path){throw "Missing source $name"}
    $obj=Join-Path $build "$name.obj"; $objects+=$obj
    & cl.exe @flags /c $path "/Fo$obj" 2>&1 | Out-File "$name.compile.log" -Encoding utf8
    if($LASTEXITCODE){Get-Content "$name.compile.log" | Select-String 'error'; throw "Compile failed: $name"}
  }
  & link.exe /nologo /Brepro /INCREMENTAL:NO /SUBSYSTEM:CONSOLE,6.01 /MACHINE:X86 /OUT:sexyz.exe @objects ws2_32.lib iphlpapi.lib 2>&1 | Tee-Object -FilePath link.log
  if($LASTEXITCODE){throw 'Helper link failed'}
  & .\sexyz.exe v 2>&1 | Tee-Object -FilePath version.log
  if($LASTEXITCODE){throw 'Version probe failed'}
  & dumpbin.exe /headers /imports .\sexyz.exe | Out-File pe-imports.log -Encoding utf8
  $manifest=Get-ChildItem -LiteralPath $source -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($source.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
  }
  [ordered]@{pin='d83fbe2a41580e37314ea671bb54fd9fedb338cd';profile='SFX-MESSAGE-RECEIVE 4';binary_sha256=(Get-FileHash sexyz.exe).Hash;files=$manifest} | ConvertTo-Json -Depth 6 | Out-File manifest.json -Encoding utf8
  $testFlags=@('/nologo','/std:c++17','/EHsc','/MT','/W4','/DWIN32','/DHAS_STDINT_H','/DHAS_INTTYPES_H','/D_WIN32_WINNT=0x601','/D_CRT_SECURE_NO_WARNINGS')+($dirs | ForEach-Object {"/I$_"})
  $testObjects=$objects | Where-Object { (Split-Path $_ -Leaf) -ne 'sexyz.obj' }
  & cl.exe @testFlags (Join-Path $PSScriptRoot 'protocol-tests.cpp') @testObjects /Fe:protocol-tests.exe /link ws2_32.lib iphlpapi.lib /SUBSYSTEM:CONSOLE,6.01 2>&1 | Tee-Object test-build.log
  if($LASTEXITCODE){throw 'Protocol tests build failed'}
  New-Item -ItemType Directory -Path fixtures | Out-Null
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'hostile-sexyz.ini') -Destination sexyz.ini
  if($BuildOnly){Write-Output 'Build-only identity: no protocol test pass claimed'; return}
  & .\protocol-tests.exe (Join-Path $build 'sexyz.exe') (Join-Path $build 'fixtures') (Join-Path $repo 'research/architecture/evidence/section8b-owner-findings-20261009/supplied-upload-inputs') 2>&1 | Tee-Object protocol-tests.log
  if($LASTEXITCODE){throw 'Protocol tests failed'}
  $oldHelper=Join-Path $repo 'build/section8b-helper-client-compat/r7/sexyz.exe'
  if((Get-FileHash -LiteralPath $oldHelper).Hash -ne '8392825C8430E53173074FB3A9E1BDBB72448D82F9C3925E9506E74802C7AC7D'){throw 'Old control identity changed'}
  New-Item -ItemType Directory -Path old-control-fixtures | Out-Null
  & .\protocol-tests.exe $oldHelper (Join-Path $build 'old-control-fixtures') (Join-Path $repo 'research/architecture/evidence/section8b-owner-findings-20261009/supplied-upload-inputs') --old-empty-control 2>&1 | Tee-Object old-empty-control.log
  if($LASTEXITCODE){throw 'Exact old-helper negative control failed'}
  if((Get-FileHash -LiteralPath $oldHelper).Hash -ne '8392825C8430E53173074FB3A9E1BDBB72448D82F9C3925E9506E74802C7AC7D'){throw 'Old control changed during qualification'}
  Get-FileHash sexyz.exe
} finally {Pop-Location}
