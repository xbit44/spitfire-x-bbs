param([Parameter(Mandatory=$true)][string]$Repository,[Parameter(Mandatory=$true)][string]$Stamp)
$ErrorActionPreference='Stop'
$directory=Join-Path $Repository 'build/section8b-helper-empty-batch/b5'
$manifestPath=Join-Path $directory 'manifest.json'
if ((Get-FileHash -LiteralPath $manifestPath).Hash -ne 'CF4FED05C35114953C4743BB1F209B990A06A8C52D93B152CFA0FD4CADF8D0AE') {throw 'Exact message helper build binding changed'}
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$materials=Join-Path $Repository 'research/architecture/section8b-helper-empty-batch'
if (@(Get-ChildItem -LiteralPath $materials -File -Recurse).Count -ne $manifest.inputs.Count) {throw 'Helper source inventory changed'}
foreach ($entry in $manifest.inputs) {
    $path=Join-Path $materials $entry.path
    if ((Get-Item -LiteralPath $path).Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $path).Hash -ne $entry.sha256) {throw "Helper input changed: $($entry.path)"}
}
$qualificationPath=Join-Path $directory 'qualification.json'
if ((Get-FileHash -LiteralPath $qualificationPath).Hash -ne $manifest.qualification_sha256) {throw 'Helper qualification changed'}
$qualification=Get-Content -LiteralPath $qualificationPath -Raw | ConvertFrom-Json
$helper=Join-Path $directory $manifest.binary
if ($qualification.result -ne 'PASS' -or $qualification.helper_sha256 -ne $manifest.sha256 -or (Get-FileHash -LiteralPath $helper).Hash -ne $manifest.sha256) {throw 'Helper differs from the exact qualified image'}
[IO.File]::WriteAllText($Stamp,$manifest.sha256.ToLowerInvariant()+"`n",[Text.Encoding]::ASCII)
