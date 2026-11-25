@echo off
setlocal
if "%~1"=="" (
  if exist "%~dp0/sfx_storage_infrastructure_tests.exe" (
    set "SFX_STAGE5A_BIN=%~dp0"
  ) else (
    set "SFX_STAGE5A_BIN=%~dp0../build/windows-v143-x86/Release"
  )
) else (
  set "SFX_STAGE5A_BIN=%~f1"
)

echo SFX Stage 5A Windows runtime qualification
echo Date: %DATE% %TIME%
ver
where wmic.exe >nul 2>&1
if not errorlevel 1 wmic.exe os get Caption,Version,BuildNumber,OSArchitecture /value

if not exist "%SFX_STAGE5A_BIN%/sfx_storage_infrastructure_tests.exe" (
  echo ERROR: storage qualification executable not found in "%SFX_STAGE5A_BIN%".
  exit /b 2
)
if not exist "%SFX_STAGE5A_BIN%/sfx_win32_storage_path_tests.exe" (
  echo ERROR: path qualification executable not found in "%SFX_STAGE5A_BIN%".
  exit /b 2
)

certutil.exe -hashfile "%SFX_STAGE5A_BIN%/sfx_storage_infrastructure_tests.exe" SHA256
if errorlevel 1 exit /b 3
certutil.exe -hashfile "%SFX_STAGE5A_BIN%/sfx_win32_storage_path_tests.exe" SHA256
if errorlevel 1 exit /b 3

"%SFX_STAGE5A_BIN%/sfx_storage_infrastructure_tests.exe" "%TEMP%/stage5a-storage-tests"
if errorlevel 1 exit /b 10
"%SFX_STAGE5A_BIN%/sfx_win32_storage_path_tests.exe" "%TEMP%/stage5a-win32-path-test"
if errorlevel 1 exit /b 11

echo SFX Stage 5A Windows runtime qualification PASSED.
exit /b 0
