param([Parameter(Mandatory=$true)][string]$Repository,[Parameter(Mandatory=$true)][string]$Stamp)
$ErrorActionPreference='Stop'
$packet8dManifestPath=Join-Path $Repository 'build/rc10-helper-packet/r2/manifest.json'
if((Get-FileHash -LiteralPath $packet8dManifestPath).Hash -ne '1190685FFE6D303B7C644D58370201A2BA1A68B2F4E1A8CD9832ED64B741B782'){throw 'Exact packet helper build binding changed'}
$packet8dManifest=Get-Content -LiteralPath $packet8dManifestPath -Raw|ConvertFrom-Json
if($packet8dManifest.upstream -ne 'd83fbe2a41580e37314ea671bb54fd9fedb338cd' -or $packet8dManifest.inputs.Count -ne80 -or $packet8dManifest.sha256 -ne '981A34ADBBEE712E04FE39E4A087C938763D78A25C288749A64FAB05E8D049EB'){throw 'Packet helper source/purpose identity mismatch'}
$packet8dSource=Join-Path $Repository 'research/architecture/rc10-packet-helper'
$packet8dActual=@(Get-ChildItem -LiteralPath $packet8dSource -File -Recurse)
if($packet8dActual.Count -ne80){throw 'Packet helper corresponding-material inventory changed'}
foreach($packet8dInput in $packet8dManifest.inputs){$packet8dFile=Join-Path $packet8dSource $packet8dInput.path;if((Get-Item -LiteralPath $packet8dFile).Length -ne $packet8dInput.bytes -or (Get-FileHash -LiteralPath $packet8dFile).Hash -ne $packet8dInput.sha256){throw "Exact packet input changed: $($packet8dInput.path)"}}
$packet8dQualification=Join-Path $Repository 'build/rc10-helper-packet-tests/r4/qualification.json'
if((Get-FileHash -LiteralPath $packet8dQualification).Hash -ne '26164CEDC8BFCC470C29FC81E94FA2EFC4DBE0CAF8EAF3ACE2FBED237F676448'){throw 'Exact packet helper process qualification changed'}
$packet8dQ=Get-Content -LiteralPath $packet8dQualification -Raw|ConvertFrom-Json
$packet8dHelper=Join-Path $Repository 'build/rc10-helper-packet/r2/SFXQWKTransfer.exe'
if($packet8dQ.helper_sha256 -ne $packet8dManifest.sha256 -or (Get-FileHash -LiteralPath $packet8dHelper).Hash -ne $packet8dManifest.sha256){throw 'Packet helper is not exact tested image'}
# Activation guard ONLY. Not installed/client/reader/package qualification and
# not an independent DR clearance. Never silently rebuild/substitute this image.
[IO.File]::WriteAllText($Stamp,"981a34adbbee712e04fe39e4a087c938763d78a25c288749a64fab05e8d049eb`n",[Text.Encoding]::ASCII)
