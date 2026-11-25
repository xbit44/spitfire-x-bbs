param([Parameter(Mandatory=$true)][string]$Repository,[Parameter(Mandatory=$true)][string]$Stamp)
$ErrorActionPreference='Stop'
$file10ManifestPath=Join-Path $Repository 'build/section10-helper-file/r4/manifest.json'
if((Get-FileHash -LiteralPath $file10ManifestPath).Hash -ne '5D2E066867DC04A552157208B40247E947298751A6BBA42D4DD1E136F03DA805'){throw 'Exact file helper build binding changed'}
$file10Manifest=Get-Content -LiteralPath $file10ManifestPath -Raw|ConvertFrom-Json
$file10Hash='81257FC9C96AAAE5BC03FA6EEC7DFAD2BF82A5E9938F0DB52B6ADC2417807C3B'
if($file10Manifest.upstream -ne 'd83fbe2a41580e37314ea671bb54fd9fedb338cd' -or $file10Manifest.profile -ne 'SFX-FILE-TRANSFER 1 Z; 2 X Y G' -or $file10Manifest.inputs.Count -ne 84 -or $file10Manifest.sha256 -ne $file10Hash){throw 'File helper source/profile identity mismatch'}
$file10Source=Join-Path $Repository 'research/architecture/section10-file-helper'
if(@(Get-ChildItem -LiteralPath $file10Source -File -Recurse).Count -ne 84){throw 'File helper corresponding-material inventory changed'}
foreach($entry in $file10Manifest.inputs){$source=Join-Path $file10Source $entry.path;if((Get-Item -LiteralPath $source).Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $source).Hash -ne $entry.sha256){throw "Exact file helper material changed: $($entry.path)"}}
$file10Qualifications=@(
    @{path='build/section10-g-startup-r2/result.json';sha256='0DB2BA9474C979BDC9D5D41DF2B37F6795153A482D81FA08FFEB22F2D28DA0BF'},
    @{path='build/section10-helper-tests/r5/result.json';sha256='7EE90598AE9DDFFC1A1932A68B005E1BE4C06E9DA2ED4F8AF14EEAE5C13B9446'},
    @{path='build/section10-helper-zregression/r3/result.json';sha256='CF0139351698F8FEF8F440DA8A99E5B2BE939D37243473D6CB3316CBBA753485'}
)
foreach($entry in $file10Qualifications){
    $path=Join-Path $Repository $entry.path
    if((Get-FileHash -LiteralPath $path).Hash -ne $entry.sha256){throw 'File helper process qualification changed'}
    $qualification=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    if($qualification.result -ne 'PASS' -or $qualification.helper_sha256 -ne $file10Hash){throw 'File helper differs from exact qualified image'}
}
if((Get-FileHash -LiteralPath (Join-Path $Repository 'build/section10-helper-file/r4/SFXFileTransfer.exe')).Hash -ne $file10Hash){throw 'File helper image changed'}
# Activation guard, not new qualification or permission to substitute a helper.
[IO.File]::WriteAllText($Stamp,$file10Hash.ToLowerInvariant()+"`n",[Text.Encoding]::ASCII)
