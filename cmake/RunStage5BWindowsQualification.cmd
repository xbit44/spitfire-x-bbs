@echo off
setlocal

set "SFX_STAGE5B_BIN=%~dp0"
if exist "%SFX_STAGE5B_BIN%sfx_storage_account_tests.exe" goto :located
if exist "%SFX_STAGE5B_BIN%Release\sfx_storage_account_tests.exe" set "SFX_STAGE5B_BIN=%SFX_STAGE5B_BIN%Release\"

:located
echo SFX Stage 5B Windows runtime qualification
echo Date: %DATE% %TIME%
ver
echo PROCESSOR_ARCHITECTURE=%PROCESSOR_ARCHITECTURE%
echo PROCESSOR_ARCHITEW6432=%PROCESSOR_ARCHITEW6432%
reg.exe query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v ProductName
reg.exe query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber
echo.

for %%F in (
  sfx_storage_infrastructure_tests.exe
  sfx_storage_account_tests.exe
  sfx_storage_reader_pool_measurements.exe
  sfx_storage_constrained_measurements.exe
  sfx_win32_storage_path_tests.exe
  sfx_win32_storage_dacl_tests.exe
) do (
  if not exist "%SFX_STAGE5B_BIN%%%F" (
    echo ERROR: required qualification executable not found: "%SFX_STAGE5B_BIN%%%F"
    exit /b 2
  )
  certutil.exe -hashfile "%SFX_STAGE5B_BIN%%%F" SHA256
  if errorlevel 1 exit /b 3
)

set "SFX_STAGE5B_TMP=%TEMP%\sfx-stage5b-qualification-%RANDOM%-%RANDOM%"
mkdir "%SFX_STAGE5B_TMP%"
if errorlevel 1 exit /b 4

"%SFX_STAGE5B_BIN%sfx_storage_infrastructure_tests.exe" "%SFX_STAGE5B_TMP%\stage5a-storage-tests"
if errorlevel 1 goto :failed
"%SFX_STAGE5B_BIN%sfx_storage_account_tests.exe" "%SFX_STAGE5B_TMP%\accounts"
if errorlevel 1 goto :failed
"%SFX_STAGE5B_BIN%sfx_storage_reader_pool_measurements.exe" "%SFX_STAGE5B_TMP%\readers"
if errorlevel 1 goto :failed
"%SFX_STAGE5B_BIN%sfx_storage_constrained_measurements.exe" "%SFX_STAGE5B_TMP%\constrained"
if errorlevel 1 goto :failed
"%SFX_STAGE5B_BIN%sfx_win32_storage_path_tests.exe" "%SFX_STAGE5B_TMP%\path"
if errorlevel 1 goto :failed
"%SFX_STAGE5B_BIN%sfx_win32_storage_dacl_tests.exe" "%SFX_STAGE5B_TMP%\dacl"
if errorlevel 1 goto :failed

rmdir "%SFX_STAGE5B_TMP%" 2>nul
echo SFX Stage 5B Windows runtime qualification PASSED.
exit /b 0

:failed
set "SFX_STAGE5B_RESULT=%ERRORLEVEL%"
echo SFX Stage 5B Windows runtime qualification FAILED with exit code %SFX_STAGE5B_RESULT%.
echo Disposable evidence remains at "%SFX_STAGE5B_TMP%".
exit /b %SFX_STAGE5B_RESULT%
