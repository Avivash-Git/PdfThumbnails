@echo off
rem Configures and builds one CMake preset with MSVC (CI runs the same script):
rem   scripts\build.cmd <preset>      x64-release | x64-debug | arm64-release | ci-x64 | ci-arm64
rem Finds Visual Studio with vswhere, sets up the compiler environment with vcvarsall, then runs
rem "cmake --workflow --preset <preset>" in the repository root.
rem Optional: set PDFT_VS_VERSION to a vswhere version range, e.g. [18.0,19.0), to pin the
rem Visual Studio version (CI images can carry more than one).
setlocal EnableExtensions DisableDelayedExpansion

set "PRESET=%~1"
if not defined PRESET goto usage
if not "%~2"=="" goto usage

rem Target from the preset name, host from the OS (x64 cmd on ARM64 reports PROCESSOR_ARCHITEW6432).
set "TARGET=x64"
echo(%PRESET%| findstr /i "arm64" >nul && set "TARGET=arm64"
set "HOST=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOST=arm64"
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set "HOST=arm64"
set "VCARCH=%HOST%_%TARGET%"
if "%HOST%"=="%TARGET%" set "VCARCH=%TARGET%"

set "REQUIRES=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
if "%TARGET%"=="arm64" set "REQUIRES=%REQUIRES% Microsoft.VisualStudio.Component.VC.Tools.ARM64"
set "VSRANGE="
if defined PDFT_VS_VERSION set VSRANGE=-version "%PDFT_VS_VERSION%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
rem vswhere's path contains "(x86)", which breaks "for /f" command quoting: go through a file.
set "VSWHERE_OUT=%TEMP%\pdft-vswhere-%RANDOM%%RANDOM%.txt"
"%VSWHERE%" -latest -products * %VSRANGE% -requires %REQUIRES% -property installationPath > "%VSWHERE_OUT%"
set "VSI="
set /p VSI=< "%VSWHERE_OUT%"
del "%VSWHERE_OUT%" >nul 2>nul
if not defined VSI goto novs
echo Visual Studio: %VSI%

rem VsDevCmd runs "vswhere.exe" from the Installer folder by bare name, which fails when
rem NoDefaultCurrentDirectoryInExePath is set; put that folder on PATH.
for %%I in ("%VSWHERE%") do set "PATH=%PATH%;%%~dpI"
set "SPECTRE="
echo(%PRESET%| findstr /b /i "ci-" >nul && set "SPECTRE=-vcvars_spectre_libs=spectre"
call "%VSI%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% %SPECTRE%
if errorlevel 1 (
  echo vcvarsall.bat %VCARCH% %SPECTRE% failed
  exit /b 1
)

where cmake >nul 2>nul || goto nocmake
where ninja >nul 2>nul || goto nocmake
cmake --version | findstr /b /c:"cmake version"
cl 2>&1 | findstr /c:"Version"

pushd "%~dp0.." || exit /b 1
cmake --workflow --preset "%PRESET%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

:usage
echo usage: scripts\build.cmd ^<preset^>
echo   presets: x64-release, x64-debug, arm64-release, ci-x64, ci-arm64
exit /b 2

:novs
echo Visual Studio with the C++ %TARGET% tools was not found (vswhere -requires %REQUIRES% %VSRANGE%).
echo Install Visual Studio 2022 or 2026 (Build Tools are enough) with the components in .vsconfig.
exit /b 1

:nocmake
echo cmake or ninja is not on PATH: install the "C++ CMake tools for Windows" component (.vsconfig).
exit /b 1
