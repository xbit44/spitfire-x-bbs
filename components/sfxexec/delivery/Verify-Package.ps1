$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$taskPrefix = [IO.Path]::GetFullPath($taskRoot) + [IO.Path]::DirectorySeparatorChar
$taskCount = 0
foreach ($taskLine in [IO.File]::ReadAllLines((Join-Path $taskRoot 'SHA256SUMS.txt'))) {
    if ($taskLine -notmatch '^([A-F0-9]{64})  (.+)$') { throw 'Invalid package checksum line' }
    $taskExpected = $Matches[1]
    $taskPath = [IO.Path]::GetFullPath((Join-Path $taskRoot $Matches[2]))
    if (-not $taskPath.StartsWith($taskPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Package path outside candidate' }
    $taskStream = [IO.File]::OpenRead($taskPath)
    $taskSha = [Security.Cryptography.SHA256]::Create()
    try { $taskActual = [BitConverter]::ToString($taskSha.ComputeHash($taskStream)).Replace('-', '') }
    finally { $taskStream.Dispose(); $taskSha.Dispose() }
    if ($taskActual -ne $taskExpected) { throw ('Checksum mismatch: ' + $taskPath) }
    $taskCount++
}
Write-Host ('PASS: all ' + $taskCount + ' package file checksums match. Native DOS/coexistence remains pending.')
