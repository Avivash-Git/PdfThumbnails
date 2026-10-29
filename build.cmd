@echo off
rem Builds PdfThumbnails (see build.ps1) without changing the PowerShell execution policy.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
