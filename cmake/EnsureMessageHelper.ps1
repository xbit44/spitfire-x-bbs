param([Parameter(Mandatory=$true)][string]$Repository,[Parameter(Mandatory=$true)][string]$Stamp)
$ErrorActionPreference='Stop'
$review=Join-Path $Repository 'research/architecture/evidence/section8b-helper-empty-batch-20261009/r1/DR_EXACT_HELPER_REVIEW.json'
if((Get-FileHash -LiteralPath $review).Hash -ne '1C9F6D021BE26E2A93DB33FD597763489AFA71ADD067F00D286B479353308F0E'){throw 'Exact helper integration clearance changed'}
$materials=Join-Path $Repository 'research/architecture/evidence/section8b-helper-empty-batch-20261009/r1/source-materials.json'
if((Get-FileHash -LiteralPath $materials).Hash -ne '00D87E25D09D7479FA68420140BB83B659FB1947887FEA5A55D4FB58680DCAEB'){throw 'Approved helper materials inventory changed'}
$source=Join-Path $Repository 'research/architecture/section8b-helper-empty-batch'
$expected=@(Get-Content -LiteralPath $materials -Raw | ConvertFrom-Json)
if($expected.Count -ne 75){throw 'Approved helper material count changed'}
foreach($item in $expected){
    $path=Join-Path $source $item.path
    if((Get-Item -LiteralPath $path).Length -ne $item.bytes -or (Get-FileHash -LiteralPath $path).Hash -ne $item.sha256){throw "Approved helper input differs: $($item.path)"}
}
$helper=Join-Path $Repository 'build/section8b-helper-empty-batch/r4/sexyz.exe'
# Never silently substitute or rebuild a differently identified helper.
if((Get-FileHash -LiteralPath $helper).Hash -ne 'E884051DC99FA4D56DF3770CD2680EC40A22C32048440354CCE8D4E9E0D36FE8'){throw 'Helper is not the exact qualified candidate'}
# Generated engineering stamp, not a source change or installation mutation.
[IO.File]::WriteAllText($Stamp,"e884051dc99fa4d56df3770cd2680ec40a22c32048440354cce8d4e9e0d36fe8`n",[Text.Encoding]::ASCII)
