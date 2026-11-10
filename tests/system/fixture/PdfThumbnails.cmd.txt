@echo off
rem Runs PdfThumbnails.ps1 in 64-bit Windows PowerShell without changing the execution policy.
rem   PdfThumbnails.cmd [status ^| install ^| repair ^| zoom N ^| refresh ^| uninstall] [options]
rem Double-clicked (no arguments) it shows the status and waits for a key.
setlocal
set "PSEXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
rem From a 32-bit host, System32 would be redirected to 32-bit PowerShell.
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PSEXE=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PSEXE%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0PdfThumbnails.ps1" %*
set "rc=%errorlevel%"
if "%~1"=="" pause
exit /b %rc%
