param([Parameter(Mandatory=$true)][string]$Repository,[Parameter(Mandatory=$true)][string]$Stamp)
$ErrorActionPreference='Stop'
$directory=Join-Path $Repository 'build/rc10-helper-packet/b5'
$manifestPath=Join-Path $directory 'manifest.json'
if ((Get-FileHash -LiteralPath $manifestPath).Hash -ne '68DD4720ED08C1392215AF61F3BF124FC937919C767D8BCBBCF777E86D10C7F5') {throw 'Exact packet helper build binding changed'}
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$materials=Join-Path $Repository 'research/architecture/rc10-packet-helper'
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
