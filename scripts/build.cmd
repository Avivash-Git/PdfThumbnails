@echo off
rem Configures, builds and tests one CMake preset with MSVC (CI runs the same script):
rem   scripts\build.cmd <preset>      x64-release | x64-debug | arm64-release | arm64-debug |
rem                                   x64-asan | ci-x64 | ci-arm64
rem Finds Visual Studio with vswhere, sets up the compiler environment with vcvarsall, then runs
rem "cmake --workflow --preset <preset>" in the repository root. Needs Visual Studio 2026 18.11 or
rem later (MSVC v145) and CMake 4.3 or later: vcvarsall appends Visual Studio's CMake to PATH, so
rem a CMake installed separately and already on PATH is used instead.
rem Optional: set PDFT_VS_VERSION to a vswhere version range, e.g. [18.11,19.0), to pin the
rem Visual Studio version (CI images can carry more than one; empty means the newest 18.11 or later).
rem Writes the toolchain versions to out\build\<preset>\buildinfo.txt (CI ships it as BUILDINFO.txt).
setlocal EnableExtensions DisableDelayedExpansion

set "PRESET=%~1"
if not defined PRESET goto usage
if not "%~2"=="" goto usage
rem Preset names become folder names below: letters, digits and dashes only.
echo(%PRESET%| findstr /r /x /c:"[-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789]*" >nul || goto usage

rem Target from the preset name, host from the OS (x64 cmd on ARM64 reports PROCESSOR_ARCHITEW6432).
set "TARGET=x64"
echo(%PRESET%| findstr /i "arm64" >nul && set "TARGET=arm64"
set "HOST=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOST=arm64"
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set "HOST=arm64"
set "VCARCH=%HOST%_%TARGET%"
if "%HOST%"=="%TARGET%" set "VCARCH=%TARGET%"

rem ci-* presets require the Spectre-mitigated runtime (PDFT_SPECTRE=ON): pick an install that has it.
set "CIPRESET="
echo(%PRESET%| findstr /b /i "ci-" >nul && set "CIPRESET=1"
set "REQUIRES=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
if "%TARGET%"=="arm64" set "REQUIRES=%REQUIRES% Microsoft.VisualStudio.Component.VC.Tools.ARM64"
if defined CIPRESET if "%TARGET%"=="x64" set "REQUIRES=%REQUIRES% Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre"
if defined CIPRESET if "%TARGET%"=="arm64" set "REQUIRES=%REQUIRES% Microsoft.VisualStudio.Component.VC.Runtimes.ARM64.Spectre"
set VSRANGE=-version "[18.11,)"
rem Compared, not "if defined": CI passes an unset repository variable as an empty value.
if not "%PDFT_VS_VERSION%"=="" set VSRANGE=-version "%PDFT_VS_VERSION%"

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
if defined CIPRESET set "SPECTRE=-vcvars_spectre_libs=spectre"
call "%VSI%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% %SPECTRE%
if errorlevel 1 (
  echo vcvarsall.bat %VCARCH% %SPECTRE% failed
  exit /b 1
)

where cmake >nul 2>nul || goto nocmake
where ninja >nul 2>nul || goto nocmake

pushd "%~dp0.." || exit /b 1
rem The folder is CMakePresets.json's binaryDir (out/build/${presetName}).
if not exist "out\build\%PRESET%" mkdir "out\build\%PRESET%"
set "INFO=out\build\%PRESET%\buildinfo.txt"
> "%INFO%" echo(preset: %PRESET%
>> "%INFO%" echo(visual_studio: %VSI%
>> "%INFO%" echo(visual_studio_version: %VSCMD_VER%
>> "%INFO%" echo(msvc_tools: %VCToolsVersion%
>> "%INFO%" echo(windows_sdk: %WindowsSDKVersion%
for /f "delims=" %%L in ('cl 2^>^&1 ^| findstr /c:"Version"') do >> "%INFO%" echo(cl: %%L
for /f "delims=" %%L in ('link 2^>^&1 ^| findstr /c:"Version"') do >> "%INFO%" echo(link: %%L
for /f "delims=" %%L in ('cmake --version ^| findstr /b /c:"cmake version"') do >> "%INFO%" echo(cmake: %%L
for /f "delims=" %%L in ('ninja --version') do >> "%INFO%" echo(ninja: %%L
type "%INFO%"

cmake --workflow --preset "%PRESET%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

:usage
echo usage: scripts\build.cmd ^<preset^>
echo   presets: x64-release, x64-debug, arm64-release, arm64-debug, x64-asan, ci-x64, ci-arm64
exit /b 2

:novs
echo Visual Studio with the C++ %TARGET% tools was not found (vswhere -requires %REQUIRES% %VSRANGE%).
echo Install Visual Studio 2026 18.11 or later (Build Tools are enough) with the components in .vsconfig.
exit /b 1

:nocmake
echo cmake or ninja is not on PATH: install the "C++ CMake tools for Windows" component (.vsconfig).
exit /b 1
