param([ValidatePattern('^r[0-9]+$')][string]$RunName='r1')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$base=Join-Path $repo 'research/synchronet/references/sexyz/20261008-d83fbe2a/sbbs-d83fbe2a41580e37314ea671bb54fd9fedb338cd'
$old=Join-Path $repo 'research/architecture/section8b-helper-client-compat'
$receipt=Join-Path $repo "research/architecture/evidence/section8b-helper-empty-batch-20261009/$RunName"
if(Test-Path -LiteralPath $receipt){throw 'Receipt already exists; preserve all attempts'}
New-Item -ItemType Directory -Path $receipt | Out-Null
function Write-ExactPatch($origin,$prefix,$targets,$added,$destination){
    $lines=foreach($relative in $targets){
        $before=if($added -contains $relative){'/dev/null'}else{Join-Path $origin $relative}
        $after=Join-Path $PSScriptRoot ($prefix+$relative)
        $diff=& git.exe diff --no-index --no-ext-diff -- $before $after
        if($LASTEXITCODE -ne 1){throw "Expected changed source: $relative"}
        foreach($line in $diff){
            if($line -match '^diff --git '){"diff --git a/$relative b/$relative"}
            elseif($line -match '^--- '){if($added -contains $relative){'--- /dev/null'}else{"--- a/$relative"}}
            elseif($line -match '^\+\+\+ '){"+++ b/$relative"}
            else{$line}
        }
    }
    (($lines -join "`n")+"`n") | Out-File $destination -Encoding utf8 -NoNewline
}
$sourceTargets=@('src/sbbs3/sexyz.c','src/sbbs3/zmodem.c','src/sbbs3/zmodem.h','src/sbbs3/sfx_message_profile.h','src/sbbs3/sfx_message_receive.inc')
$added=@('src/sbbs3/sfx_message_profile.h','src/sbbs3/sfx_message_receive.inc')
Write-ExactPatch $base 'source/' $sourceTargets $added (Join-Path $receipt 'strict-helper.patch')
$deltaTargets=@('source/src/sbbs3/sfx_message_receive.inc','git_hash.h','git_branch.h')
Write-ExactPatch $old '' $deltaTargets @() (Join-Path $receipt 'approved-to-empty-delta.patch')
$materials=Get-ChildItem -LiteralPath $PSScriptRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($PSScriptRoot.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
}
$materials | ConvertTo-Json -Depth 5 | Out-File (Join-Path $receipt 'source-materials.json') -Encoding utf8
$hashes=@()
foreach($run in @('r3','r4')){
    $build=Join-Path $repo "build/section8b-helper-empty-batch/$run"
    $identity=Get-Content (Join-Path $build 'manifest.json') -Raw | ConvertFrom-Json
    $hash=(Get-FileHash (Join-Path $build 'sexyz.exe')).Hash
    if($identity.binary_sha256 -ne $hash -or $identity.profile -ne 'SFX-MESSAGE-RECEIVE 4'){throw 'Build identity mismatch'}
    foreach($file in $identity.files){if((Get-FileHash (Join-Path $PSScriptRoot ('source/'+$file.path))).Hash -ne $file.sha256){throw 'Built inputs changed'}}
    if((Get-Content (Join-Path $build 'protocol-tests.log') -Raw) -notmatch 'PASS 146 parser/data cases and 369 external helper processes;'){throw 'Required full helper qualification not passed'}
    if((Get-Content (Join-Path $build 'old-empty-control.log') -Raw) -notmatch 'PASS old exact helper empty-batch negative control: 3 external helper processes;'){throw 'Exact old-helper negative control missing'}
    $hashes+=$hash
    foreach($name in @('manifest.json','protocol-tests.log','pe-imports.log','test-build.log','old-empty-control.log')){
        Copy-Item -LiteralPath (Join-Path $build $name) -Destination (Join-Path $receipt ($run+'-'+$name))
    }
}
if($hashes[0] -ne $hashes[1]){throw 'Independent builds differ'}
$reconstruction=Join-Path $receipt 'upstream-reconstruction'
foreach($file in $identity.files){
    if($added -contains $file.path){continue}
    $target=Join-Path $reconstruction $file.path
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $base $file.path) -Destination $target
}
$relativeRoot=$reconstruction.Substring($repo.Length+1).Replace('\','/')
& git.exe -C $repo apply --check "--directory=$relativeRoot" (Join-Path $receipt 'strict-helper.patch')
if($LASTEXITCODE){throw 'Full patch check failed'}
& git.exe -C $repo apply "--directory=$relativeRoot" (Join-Path $receipt 'strict-helper.patch')
if($LASTEXITCODE){throw 'Full patch reconstruction failed'}
foreach($file in $identity.files){if((Get-FileHash (Join-Path $reconstruction $file.path)).Hash -ne $file.sha256){throw 'Full corresponding source mismatch'}}
$deltaRoot=Join-Path $receipt 'approved-reconstruction'
foreach($file in $deltaTargets){
    $target=Join-Path $deltaRoot $file
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $old $file) -Destination $target
}
$relativeRoot=$deltaRoot.Substring($repo.Length+1).Replace('\','/')
& git.exe -C $repo apply --check "--directory=$relativeRoot" (Join-Path $receipt 'approved-to-empty-delta.patch')
if($LASTEXITCODE){throw 'Approved delta patch check failed'}
& git.exe -C $repo apply "--directory=$relativeRoot" (Join-Path $receipt 'approved-to-empty-delta.patch')
if($LASTEXITCODE){throw 'Approved delta reconstruction failed'}
foreach($file in $deltaTargets){if((Get-FileHash (Join-Path $deltaRoot $file)).Hash -ne (Get-FileHash (Join-Path $PSScriptRoot $file)).Hash){throw 'Approved delta mismatch'}}
$oldManifest=Join-Path $repo 'research/architecture/evidence/section8b-helper-client-compat-20261009/r1/source-materials.json'
if((Get-FileHash -LiteralPath $oldManifest).Hash -ne '7F71C317FF015CEC02DED70117FF3CA558DFD0D73D9B6BF33EFC4DE389828F62'){throw 'Approved manifest changed'}
$oldMaterials=Get-Content $oldManifest -Raw | ConvertFrom-Json
foreach($file in $oldMaterials){if((Get-FileHash (Join-Path $old $file.path)).Hash -ne $file.sha256){throw 'Approved old helper material changed'}}
[ordered]@{binary_sha256=$hashes[0];runs=@('r3','r4');profile=4;parser_cases_each=146;process_cases_each=369;old_negative_controls_each=3;reconstructed_files=$identity.files.Count;delta_reconstructed_files=$deltaTargets.Count;original_materials=$oldMaterials.Count;candidate_materials=$materials.Count;full_patch=(Get-FileHash (Join-Path $receipt 'strict-helper.patch')).Hash;delta_patch=(Get-FileHash (Join-Path $receipt 'approved-to-empty-delta.patch')).Hash;source_materials=(Get-FileHash (Join-Path $receipt 'source-materials.json')).Hash;activation='NOT APPROVED; shipping pins unchanged'} | ConvertTo-Json | Out-File (Join-Path $receipt 'review-identity.json') -Encoding utf8
Get-Content (Join-Path $receipt 'review-identity.json')
