param([ValidatePattern('^r[0-9]+$')][string]$RunName='r1')
$ErrorActionPreference='Stop'
# Only the new candidate recipe may run; inherited output roots are excluded.
& (Join-Path $PSScriptRoot 'make-client-review-receipt.ps1') -RunName $RunName
