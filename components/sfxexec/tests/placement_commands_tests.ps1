# EXACT manual document blocks, only new owned fixture paths. Never execute
# document's SystemDirectory resolution/owner path setup.
param([string]$FixtureRoot,[string]$CorePath,[string]$Document)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath($FixtureRoot)
if(Test-Path -LiteralPath $taskRoot){throw 'Fresh fixture directory required'}
if($taskRoot -notlike '*\build\sfxexec-system32-placement-*'){throw 'Owned build fixture root required'}
[void][IO.Directory]::CreateDirectory($taskRoot)
$text=[IO.File]::ReadAllText($Document)
$start=$text.IndexOf('BEGIN PLACE-BODY')+'BEGIN PLACE-BODY'.Length
$place=[scriptblock]::Create($text.Substring($start,$text.IndexOf('END PLACE-BODY')-$start))
$start=$text.IndexOf('BEGIN ROLLBACK-BODY')+'BEGIN ROLLBACK-BODY'.Length
$rollback=[scriptblock]::Create($text.Substring($start,$text.IndexOf('END ROLLBACK-BODY')-$start))
$expected='D36589BDE11134B7F6019C32667B639BC465CA39CD39E22FEB6E053A2996A3F3'
$script:checks=0
function Check($condition,$why){$script:checks++;if(-not $condition){throw $why}}
function Refuses([scriptblock]$action){$caught=$false;try{& $action}catch{$caught=$true};Check $caught 'Expected refusal'}
foreach($kind in @('new','reuse','conflict','sourcewrong','receipt','replaced','busy','pending')) {
    $dir=Join-Path $taskRoot $kind;[void][IO.Directory]::CreateDirectory($dir)
    $source=Join-Path $dir 'source.dll';$target=Join-Path $dir 'SFXEXEC.DLL';$receipt=Join-Path $dir 'receipt.txt'
    [IO.File]::Copy($CorePath,$source,$false)
    if($kind -eq 'reuse'){[IO.File]::Copy($source,$target,$false)}
    if($kind -eq 'conflict'){[IO.File]::WriteAllText($target,'retain existing different file')}
    if($kind -eq 'sourcewrong'){[IO.File]::AppendAllText($source,'wrong')}
    if($kind -eq 'receipt'){[IO.File]::WriteAllText($receipt,'retain prior receipt')}
    if($kind -in @('conflict','sourcewrong','receipt')) {
        Refuses {& $place}
        if($kind -eq 'conflict'){Check ([IO.File]::ReadAllText($target) -ceq 'retain existing different file') 'Conflict preserved'}
        else {Check (-not [IO.File]::Exists($target)) 'Failure before target creation'}
        continue
    }
    & $place
    $lines=[IO.File]::ReadAllLines($receipt);Check ($lines[3] -ceq 'verified') 'Verified receipt'
    Check ($lines[4] -ceq $(if($kind -eq 'reuse'){'false'}else{'true'})) 'Accurate introduced flag'
    if($kind -eq 'replaced'){[IO.File]::WriteAllText($target,'replacement');Refuses {& $rollback};Check ([IO.File]::ReadAllText($target) -ceq 'replacement') 'Different replacement retained';continue}
    if($kind -eq 'pending'){[IO.File]::WriteAllText($receipt,([IO.File]::ReadAllText($receipt).Replace('verified','pending')));Refuses {& $rollback};Check ([IO.File]::Exists($target)) 'Pending receipt cannot remove target';continue}
    if($kind -eq 'busy'){$pin=[IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read);try{Refuses {& $rollback};Check ([IO.File]::Exists($target)) 'File in use preserved'}finally{$pin.Dispose()}}
    & $rollback
    Check ([IO.File]::Exists($target) -eq ($kind -eq 'reuse')) 'Rollback removes only introduced exact file'
    Check ([IO.File]::Exists($receipt)) 'Receipt retained'
}
Write-Output "PASS $script:checks exact manual placement/rollback block checks; owned temporary files ONLY"
