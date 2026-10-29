<#
.SYNOPSIS
    Builds PdfThumbnails.dll and the thumbtest tool with LLVM-MinGW and lays out the
    installable package in dist\PdfThumbnails.

.DESCRIPTION
    Needs LLVM-MinGW (winget install --id MartinStorsjo.LLVM-MinGW.UCRT -e) and the PDFium
    binaries in third_party\pdfium, which are downloaded and checked against the pinned
    SHA-256 below when missing (or with -FetchPdfium). After building, install or update with
    the same options as the existing install, for example:
        dist\PdfThumbnails\PdfThumbnails.cmd install -Machine -AutoRepair
#>
param(
    # Download the pinned PDFium release into third_party\pdfium even if it is present.
    [switch]$FetchPdfium
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Pinned PDFium build (non-V8, non-XFA) from https://github.com/bblanchon/pdfium-binaries
$PdfiumTag    = 'chromium/8125'
$PdfiumAsset  = 'pdfium-win-x64.tgz'
$PdfiumSha256 = '9A964261760CFF52570AABF038AB346D3A5034DD8B30D0D5FE8861235D356B8B'
# The handler loads PDFium under this private name (kPdfiumDll in the source).
$PdfiumDll    = 'PdfThumbnails-pdfium.dll'

$root    = $PSScriptRoot
$pdfium  = Join-Path $root 'third_party\pdfium'
$obj     = Join-Path $root 'build'
$package = Join-Path $root 'dist\PdfThumbnails'
$tools   = Join-Path $root 'dist\tools'

function Find-Tool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    # A fresh winget install is not on this shell's PATH until a new session starts.
    $packages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    $hit = Get-ChildItem -LiteralPath $packages -Directory -Filter 'MartinStorsjo.LLVM-MinGW*' -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Recurse -Filter $Name -ErrorAction SilentlyContinue } |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    throw "$Name not found. Install LLVM-MinGW first: winget install --id MartinStorsjo.LLVM-MinGW.UCRT -e"
}

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Exe) failed with exit code $LASTEXITCODE" }
}

# Names of the DLLs a PE file imports.
function Get-Imports([string]$File) {
    & $script:readobj --coff-imports $File | Select-String '^\s*Name:\s*(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
}

# Fails unless every import is a Windows system DLL (or an api-ms-win-* API set).
function Assert-SystemImports([string]$File) {
    $imports = @(Get-Imports $File)
    Write-Host "$(Split-Path -Leaf $File) imports: $($imports -join ', ')"
    $foreign = @($imports | Where-Object { $_ -notmatch '^api-ms-win-' -and
        -not (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\$_")) })
    if ($foreign.Count) { throw "$(Split-Path -Leaf $File) depends on non-system DLLs: $($foreign -join ', ')" }
}

if ($FetchPdfium -or -not (Test-Path -LiteralPath (Join-Path $pdfium 'bin\pdfium.dll'))) {
    $url = 'https://github.com/bblanchon/pdfium-binaries/releases/download/' +
        [uri]::EscapeDataString($PdfiumTag) + "/$PdfiumAsset"
    $download = Join-Path ([IO.Path]::GetTempPath()) ('pdfium-' + [guid]::NewGuid() + '.tgz')
    Write-Host "Downloading $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing
    $hash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash
    if ($hash -ne $PdfiumSha256) {
        Remove-Item -LiteralPath $download
        throw "PDFium download has SHA-256 $hash, expected $PdfiumSha256"
    }
    if (Test-Path -LiteralPath $pdfium) { Remove-Item -LiteralPath $pdfium -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $pdfium | Out-Null
    Invoke-Native (Join-Path $env:WINDIR 'System32\tar.exe') @('-xzf', $download, '-C', $pdfium)
    Remove-Item -LiteralPath $download
}

$cxx = Find-Tool 'x86_64-w64-mingw32-clang++.exe'
$windres = Find-Tool 'x86_64-w64-mingw32-windres.exe'
$script:readobj = Find-Tool 'llvm-readobj.exe'
Write-Host "Compiler: $cxx"

New-Item -ItemType Directory -Force -Path $obj, (Join-Path $package 'bin'), (Join-Path $package 'licenses'), $tools | Out-Null
$common = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-DUNICODE', '-D_UNICODE', '-DNOMINMAX',
    '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-ffunction-sections', '-fdata-sections', '-static', '-s',
    '-Wl,--gc-sections', '-Wl,--dynamicbase', '-Wl,--nxcompat', '-Wl,--high-entropy-va')

$dll = Join-Path $package 'bin\PdfThumbnails.dll'
$source = Join-Path $root 'src\PdfThumbnails.cpp'
Invoke-Native $windres @((Join-Path $root 'src\version.rc'), '-O', 'coff', '-o', (Join-Path $obj 'version.res'))
Invoke-Native $cxx ($common + @('-fno-exceptions', '-fno-rtti', '-isystem', (Join-Path $pdfium 'include'),
    '-shared', '-o', $dll, $source, (Join-Path $root 'src\PdfThumbnails.def'), (Join-Path $obj 'version.res'),
    '-lole32', '-luuid', '-lgdi32', '-lshell32', '-ladvapi32'))
Invoke-Native $cxx ($common + @('-municode', '-o', (Join-Path $tools 'thumbtest.exe'),
    (Join-Path $root 'tools\thumbtest.cpp'), '-lole32', '-luuid', '-lshell32', '-lshlwapi', '-lgdi32',
    '-lwindowscodecs'))

# Gates: the DLL runs inside Explorer's thumbnail process.
$exports = @(& $script:readobj --coff-exports $dll | Select-String '^\s*Name:\s*(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object)
if (($exports -join ',') -ne 'DllCanUnloadNow,DllGetClassObject') { throw "Unexpected exports: $($exports -join ', ')" }
Assert-SystemImports $dll
$pdfiumSource = Join-Path $pdfium 'bin\pdfium.dll'
Assert-SystemImports $pdfiumSource
$pdfiumExports = @(& $script:readobj --coff-exports $pdfiumSource | Select-String '^\s*Name:\s*(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
$bound = @(Select-String -LiteralPath $source -Pattern 'Resolve\(dll, "(\w+)"' -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value })
$missing = @($bound | Where-Object { $pdfiumExports -notcontains $_ })
if ($missing.Count) { throw "pdfium.dll lacks: $($missing -join ', ')" }
Write-Host "pdfium.dll $((Get-Item -LiteralPath $pdfiumSource).VersionInfo.ProductVersion) exports all $($bound.Count) functions the handler binds"

Copy-Item -LiteralPath $pdfiumSource -Destination (Join-Path $package "bin\$PdfiumDll") -Force
foreach ($name in 'PdfThumbnails.ps1', 'PdfThumbnails.cmd', 'README.md', 'LICENSE') {
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination $package -Force
}
Copy-Item -LiteralPath (Join-Path $pdfium 'LICENSE') -Destination (Join-Path $package 'licenses\pdfium-binaries-LICENSE.txt') -Force
# PDFium's own notice and its third-party notices must ship with the binary.
Get-ChildItem -LiteralPath (Join-Path $pdfium 'licenses') -Force |
    Copy-Item -Destination (Join-Path $package 'licenses') -Recurse -Force
foreach ($notice in 'pdfium.txt', 'freetype.txt') {
    if (-not (Test-Path -LiteralPath (Join-Path $package "licenses\$notice"))) { throw "Package is missing licenses\$notice" }
}

Get-ChildItem -LiteralPath (Join-Path $package 'bin'), $tools -File |
    ForEach-Object { '{0,12:N0}  {1}' -f $_.Length, $_.FullName.Substring($root.Length + 1) }
Write-Host "Package ready: $package"
