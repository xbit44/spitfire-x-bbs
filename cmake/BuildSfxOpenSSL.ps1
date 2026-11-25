param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$PerlExecutable,
    [Parameter(Mandatory=$true)][string]$NasmExecutable,
    [Parameter(Mandatory=$true)][string]$VisualStudioDirectory,
    [ValidateSet('Configure', 'Build')][string]$Mode = 'Build'
)
$ErrorActionPreference = 'Stop'
# This process owns its environment; no installer, registry or persistent PATH writes.
$sshVcVars = Join-Path $VisualStudioDirectory 'VC/Auxiliary/Build/vcvarsall.bat'
$sshVsEnvironment = & $env:ComSpec /d /s /c ('"' + $sshVcVars + '" x86 >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'Cannot establish the approved MSVC x86 environment' }
foreach ($sshLine in $sshVsEnvironment) {
    if ($sshLine -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:Path = (Split-Path $PerlExecutable) + ';' + (Split-Path $NasmExecutable) + ';' + $env:Path
$env:LC_ALL = 'C'
$env:LANG = 'C'
$sshPerlPlatform = & $PerlExecutable -e 'print $^O;'
if ($LASTEXITCODE -ne 0 -or $sshPerlPlatform -ne 'MSWin32') {
    throw 'The OpenSSL MSVC build requires native Windows Perl, not Cygwin/MSYS Perl'
}
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
Push-Location $BuildDirectory
try {
    if ($Mode -eq 'Configure') {
        & $PerlExecutable (Join-Path $SourceDirectory 'Configure') VC-WIN32 `
            no-shared no-module no-dso no-tests no-apps no-docs no-autoload-config `
            no-makedepend no-pinshared no-filenames '-D_WIN32_WINNT=0x0601' `
            '--prefix=C:/SFX/PrivateCrypto' '--openssldir=C:/SFX/PrivateCrypto' `
            *> 'sfx-configure.log'
        if ($LASTEXITCODE -ne 0) { throw 'OpenSSL configure failed; see sfx-configure.log' }
        & nmake /nologo build_generated *> 'sfx-generated.log'
        if ($LASTEXITCODE -ne 0) { throw 'OpenSSL header generation failed; see sfx-generated.log' }
    } else {
        # Private static libraries only; never install into the OS.
        & nmake /nologo libcrypto.lib libssl.lib *> 'sfx-build.log'
        if ($LASTEXITCODE -ne 0) { throw 'OpenSSL static libraries build failed; see sfx-build.log' }
    }
} finally {
    Pop-Location
}
