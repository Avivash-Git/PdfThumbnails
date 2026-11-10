<#
.SYNOPSIS
    Installs, checks, repairs or removes PdfThumbnails: first-page thumbnails for PDF files
    in Windows Explorer.

.DESCRIPTION
    Explorer gets thumbnails from the handler registered for the .pdf file type, not from the
    app that opens PDFs, so this works next to PdfCraft (or any other PDF app) without
    modifying it. PdfCraft's installer and updates never touch it.

    The .pdf association is per-user. The COM class is per-user too, unless -Machine registers
    it machine-wide, with the DLLs in admin-protected Program Files (one UAC prompt). Per-user
    registry changes go through WMI, so they reach your real profile even from a terminal
    whose own registry view is virtualized (a terminal inside an MSIX-packaged app).

      status          Show what Explorer uses for .pdf thumbnails and test it (default).
      install         Install or update. A machine-wide install stays machine-wide.
      repair          Re-register whatever is missing. Safe to run any time.
      zoom [percent]  Show or set the magnification (100 = whole page, up to 200).
      refresh         Rebuild Explorer's thumbnail cache (restarts Explorer).
      uninstall       Remove everything.

    machine-register and machine-unregister are internal steps that run elevated.

.EXAMPLE
    PdfThumbnails.cmd install -Machine -AutoRepair
.EXAMPLE
    PdfThumbnails.cmd zoom 120
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'install', 'repair', 'zoom', 'refresh', 'uninstall', 'machine-register', 'machine-unregister')]
    [string]$Command = 'status',

    # zoom: magnification in percent. Above 100 the page's longer side is cropped evenly.
    [Parameter(Position = 1)]
    [ValidateRange(100, 200)]
    [int]$Percent,

    # install: register the COM class machine-wide (one UAC prompt).
    [switch]$Machine,

    # install or repair: add, or with -NoAutoRepair remove, the scheduled task that runs
    # "repair" at sign-in and after every PdfCraft install or update.
    [switch]$AutoRepair,
    [switch]$NoAutoRepair,

    # status: also extract this PDF's thumbnail through Explorer's pipeline.
    [string]$TestFile,

    # Log to repair.log instead of the console (used by the scheduled task).
    [switch]$Quiet,

    # Internal, for the elevated machine-* steps: the DLL folder, and where to report.
    [string]$Source,
    [string]$Log
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'

# 32-bit PowerShell sees the WOW64 registry view and C:\Program Files (x86); Explorer is 64-bit.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    Write-Host 'Run PdfThumbnails from 64-bit PowerShell; PdfThumbnails.cmd does this for you.' -ForegroundColor Red
    exit 1
}

$Clsid        = '{0C3534C4-5B7A-48AB-828D-1CD660DC447F}'  # must match src\PdfThumbnails.cpp
$ThumbnailKey = '{e357fccd-a995-4576-b01f-234630154e96}'  # IThumbnailProvider handler slot
$ClassName    = 'PdfThumbnails PDF thumbnail provider'
$HandlerDll   = 'PdfThumbnails.dll'
$PdfiumDll    = 'PdfThumbnails-pdfium.dll'                # must match kPdfiumDll in the DLL
$InstallRoot  = Join-Path $env:LOCALAPPDATA 'Programs\PdfThumbnails'
$MachineRoot  = Join-Path $env:ProgramFiles 'PdfThumbnails'
$SettingsFile = Join-Path $InstallRoot 'settings.ini'     # read by the DLL as well
$LogFile      = Join-Path $InstallRoot 'repair.log'
$TaskName     = 'PdfThumbnails auto-repair'
$ClsidKey     = "Software\Classes\CLSID\$Clsid"           # same path under HKCU and HKLM
# Both slots point at the handler. Explorer reads the first; the second survives tools such
# as PowerToys or SumatraPDF that delete the .pdf slot on uninstall without checking its owner.
$HandlerKeys  = @(
    "Software\Classes\.pdf\ShellEx\$ThumbnailKey",
    "Software\Classes\SystemFileAssociations\.pdf\ShellEx\$ThumbnailKey"
)
$RunOnceKey   = 'Software\Microsoft\Windows\CurrentVersion\RunOnce'
$CleanupValue = 'PdfThumbnailsCleanup'
$HKLM         = [Microsoft.Win32.Registry]::LocalMachine
$Sid          = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$HKEY_USERS   = [uint32]2147483651

# ---- helpers -------------------------------------------------------------------------------

function Write-Line([string]$Text, [ConsoleColor]$Color = 'Gray') {
    if ($Log) { Add-Content -LiteralPath $Log -Encoding UTF8 -Value $Text; return }  # elevated step
    if (-not $Quiet) { Write-Host $Text -ForegroundColor $Color; return }
    New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
    if ((Test-Path -LiteralPath $LogFile) -and (Get-Item -LiteralPath $LogFile).Length -gt 256KB) {
        Move-Item -LiteralPath $LogFile -Destination "$LogFile.old" -Force
    }
    Add-Content -LiteralPath $LogFile -Encoding UTF8 -Value ('{0:yyyy-MM-dd HH:mm:ss}  {1}' -f (Get-Date), $Text)
}

function Get-ErrorText($ErrorRecord) {
    $e = $ErrorRecord.Exception
    while ($e.InnerException) { $e = $e.InnerException }
    return $e.Message
}

# Machine-wide registry, through .NET.
function Get-RegValue([Microsoft.Win32.RegistryKey]$Hive, [string]$Key, [string]$Name = '') {
    $k = $Hive.OpenSubKey($Key)
    if (-not $k) { return $null }
    try { return $k.GetValue($Name) } finally { $k.Close() }
}

# Per-user registry, through WMI's StdRegProv. It runs outside this process, so the changes
# always reach the real profile, even when this process's own view of HKCU is virtualized.
function Invoke-UserReg([string]$Method, [string]$Key, [hashtable]$More = @{}) {
    $arguments = @{ hDefKey = $HKEY_USERS; sSubKeyName = "$Sid\$Key" } + $More
    Invoke-CimMethod -Namespace 'root/default' -ClassName 'StdRegProv' -MethodName $Method -Arguments $arguments
}

function Get-UserValue([string]$Key, [string]$Name = '') {
    $r = Invoke-UserReg 'GetStringValue' $Key @{ sValueName = $Name }
    if ($r.ReturnValue -eq 0) { return $r.sValue }
    return $null
}

function Get-UserDword([string]$Key, [string]$Name) {
    $r = Invoke-UserReg 'GetDWORDValue' $Key @{ sValueName = $Name }
    if ($r.ReturnValue -eq 0) { return $r.uValue }
    return $null
}

function Set-UserValues([string]$Key, [hashtable]$Values) {
    $r = Invoke-UserReg 'CreateKey' $Key
    if ($r.ReturnValue -ne 0) { throw "Cannot create HKCU\$Key (error $($r.ReturnValue))." }
    foreach ($name in $Values.Keys) {
        $r = Invoke-UserReg 'SetStringValue' $Key @{ sValueName = $name; sValue = [string]$Values[$name] }
        if ($r.ReturnValue -ne 0) { throw "Cannot write HKCU\$Key (error $($r.ReturnValue))." }
    }
}

function Remove-UserValue([string]$Key, [string]$Name) { [void](Invoke-UserReg 'DeleteValue' $Key @{ sValueName = $Name }) }

function Remove-UserKey([string]$Key) {
    $r = Invoke-UserReg 'EnumKey' $Key
    if ($r.ReturnValue -ne 0) { return }  # not there
    foreach ($sub in @($r.sNames)) { if ($sub) { Remove-UserKey "$Key\$sub" } }
    [void](Invoke-UserReg 'DeleteKey' $Key)
}

function Remove-UserKeyIfEmpty([string]$Key) {
    $keys = Invoke-UserReg 'EnumKey' $Key
    if ($keys.ReturnValue -ne 0) { return }
    $values = Invoke-UserReg 'EnumValues' $Key
    if ($values.ReturnValue -ne 0) { return }
    if (@($keys.sNames | Where-Object { $_ }).Count -eq 0 -and @($values.sNames | Where-Object { $null -ne $_ }).Count -eq 0) {
        [void](Invoke-UserReg 'DeleteKey' $Key)
    }
}

# A value as HKEY_CLASSES_ROOT shows it: per-user entries override machine-wide ones.
function Get-ClassesValue([string]$Key, [string]$Name = '') {
    $r = Invoke-UserReg 'GetStringValue' "Software\Classes\$Key" @{ sValueName = $Name }
    if ($r.ReturnValue -eq 0) { return $r.sValue }
    return Get-RegValue $HKLM "Software\Classes\$Key" $Name
}

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Runs one of the internal machine-* commands with admin rights (UAC prompt if needed) and
# shows what it reported. Returns its exit code (-1 if it never ran).
function Invoke-Elevated([string]$Verb, [string[]]$Arguments = @()) {
    # Run the local copy, so a build folder on a mapped or subst drive does not matter.
    $script = Join-Path $InstallRoot 'PdfThumbnails.ps1'
    if (-not (Test-Path -LiteralPath $script)) { $script = $PSCommandPath }
    $report = Join-Path $InstallRoot 'elevated.log'
    New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
    Remove-Item -LiteralPath $report -ErrorAction SilentlyContinue
    $powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    # Start-Process joins the arguments with spaces, so quote the values (paths).
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"", $Verb) +
        @($Arguments + @('-Log', $report) | ForEach-Object { if ($_ -like '-*') { $_ } else { "`"$_`"" } })
    $code = -1
    if (Test-Admin) {
        & $powershell @($argList | ForEach-Object { $_.Trim('"') }) | Out-Host
        $code = $LASTEXITCODE
    } else {
        Write-Line 'Windows will ask for administrator permission (one prompt)...' Yellow
        try {
            $process = Start-Process -FilePath $powershell -ArgumentList $argList -Verb RunAs -Wait -PassThru -WindowStyle Hidden
            $code = $process.ExitCode
        } catch {
            Write-Line "Administrator step not run: $(Get-ErrorText $_)" Red
        }
    }
    if (Test-Path -LiteralPath $report) {
        Get-Content -LiteralPath $report -Encoding UTF8 | ForEach-Object { Write-Line "  $_" }
        Remove-Item -LiteralPath $report -ErrorAction SilentlyContinue
    } elseif ($code -gt 0) {
        Write-Line "  The administrator step could not run $script." Red
    }
    return $code
}

# Shell calls PowerShell cannot make on its own.
$Native = $false
try {
    Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class PdfThumbsShell
{
    [DllImport("shell32.dll")]
    private static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);

    // Tells Explorer that file associations changed, so it picks up the new handler.
    public static void AssocChanged()
    {
        SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero);  // SHCNE_ASSOCCHANGED, SHCNF_IDLIST
    }

    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
    private static extern int AssocQueryStringW(int flags, int what, string assoc, string extra,
                                                StringBuilder output, ref uint length);

    // what: 16 = ASSOCSTR_SHELLEXTENSION, 20 = ASSOCSTR_PROGID. Returns null if unset.
    public static string Query(int what, string assoc, string extra)
    {
        uint length = 512;
        StringBuilder output = new StringBuilder((int)length);
        return AssocQueryStringW(0, what, assoc, extra, output, ref length) == 0 ? output.ToString() : null;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHCreateItemFromParsingName(string path, IntPtr bindContext,
        [MarshalAs(UnmanagedType.LPStruct)] Guid iid, out IntPtr item);

    [ComImport, Guid("F676C15D-596A-4ce2-8234-33996F445DB1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IThumbnailCache
    {
        [PreserveSig] int GetThumbnail(IntPtr item, uint size, int flags, out IntPtr bitmap, IntPtr cacheFlags, IntPtr id);
    }

    [ComImport, Guid("091162a4-bc96-411f-aae8-c5122cd03363"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ISharedBitmap
    {
        [PreserveSig] int GetSharedBitmap(out IntPtr bitmap);
        [PreserveSig] int GetSize(out Size size);
        [PreserveSig] int GetFormat(out int alpha);
        [PreserveSig] int InitializeBitmap(IntPtr bitmap, int alpha);
        [PreserveSig] int Detach(out IntPtr bitmap);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Size { public int Width; public int Height; }

    // Extracts a fresh thumbnail the way Explorer does, in a new isolated thumbnail process so
    // the DLLs are really loaded (cache bypassed and left untouched). Returns the HRESULT: 0
    // when the handler worked.
    public static int ProbeExplorer(string path, int size, out int width, out int height)
    {
        width = 0;
        height = 0;
        object cache = Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("50EF4544-AC9F-4a8e-B21B-8A26180DB13F")));
        IntPtr item;
        int hr = SHCreateItemFromParsingName(path, IntPtr.Zero, new Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"), out item);
        if (hr != 0)
        {
            Marshal.ReleaseComObject(cache);
            return hr;
        }
        try
        {
            IntPtr shared;
            hr = ((IThumbnailCache)cache).GetThumbnail(item, (uint)size, 0x4 | 0x20 | 0x400, out shared, IntPtr.Zero, IntPtr.Zero);  // WTS_FORCEEXTRACTION | WTS_EXTRACTDONOTCACHE | WTS_INSTANCESURROGATE
            if (shared != IntPtr.Zero)
            {
                ISharedBitmap bitmap = (ISharedBitmap)Marshal.GetObjectForIUnknown(shared);
                Size extent;
                if (bitmap.GetSize(out extent) == 0)
                {
                    width = extent.Width;
                    height = extent.Height;
                }
                Marshal.ReleaseComObject(bitmap);
                Marshal.Release(shared);
            }
            return hr;
        }
        finally
        {
            Marshal.Release(item);
            Marshal.ReleaseComObject(cache);
        }
    }

    [DllImport("advapi32.dll")]
    private static extern bool GetTokenInformation(IntPtr token, int infoClass, out int info, int length, out int returned);

    // 2 = this process is elevated while Explorer is not (UAC admin prompt).
    public static int ElevationType()
    {
        using (System.Security.Principal.WindowsIdentity identity = System.Security.Principal.WindowsIdentity.GetCurrent())
        {
            int type, returned;
            return GetTokenInformation(identity.Token, 18, out type, 4, out returned) ? type : 0;  // TokenElevationType
        }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool MoveFileExW(string existing, string replacement, int flags);

    // Deletes a file or empty folder at the next restart (needs admin).
    public static bool DeleteAtRestart(string path)
    {
        return MoveFileExW(path, null, 4);  // MOVEFILE_DELAY_UNTIL_REBOOT
    }
}
'@
    $Native = $true
} catch {
    Write-Verbose "Shell helpers unavailable: $($_.Exception.Message)"
}

function Send-AssocChanged { if ($Native) { [PdfThumbsShell]::AssocChanged() } }

# CLSID of the thumbnail handler Windows resolves for .pdf in this process, or $null.
function Get-EffectiveHandler { if ($Native) { [PdfThumbsShell]::Query(16, '.pdf', $ThumbnailKey) } else { $null } }

function Get-ClassName([string]$Id) {
    $name = Get-ClassesValue "CLSID\$Id"
    if ($name) { return $name }
    return 'unknown handler'
}

# Extracts a thumbnail of a tiny blank PDF through Explorer's isolated thumbnail process.
function Test-ExplorerExtraction {
    $pdf = Join-Path ([IO.Path]::GetTempPath()) ('PdfThumbnails-probe-' + [guid]::NewGuid() + '.pdf')
    $objects = '<< /Type /Catalog /Pages 2 0 R >>', '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>'
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("%PDF-1.4`n")
    $offsets = foreach ($i in 0..2) { $sb.Length; [void]$sb.Append("$($i + 1) 0 obj`n$($objects[$i])`nendobj`n") }
    $xref = $sb.Length
    [void]$sb.Append("xref`n0 4`n0000000000 65535 f `n")
    foreach ($offset in $offsets) { [void]$sb.Append(('{0:D10} 00000 n ' -f $offset) + "`n") }
    [void]$sb.Append("trailer`n<< /Size 4 /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n")
    [IO.File]::WriteAllText($pdf, $sb.ToString(), [Text.Encoding]::ASCII)
    $width = 0
    $height = 0
    try { return [PdfThumbsShell]::ProbeExplorer($pdf, 96, [ref]$width, [ref]$height) }
    finally { Remove-Item -LiteralPath $pdf -ErrorAction SilentlyContinue }
}

function Get-Zoom {
    if (Test-Path -LiteralPath $SettingsFile) {
        foreach ($line in Get-Content -LiteralPath $SettingsFile) {
            if ($line -match '^\s*Zoom\s*=\s*(\d+)') { return [Math]::Max(100, [Math]::Min(200, [int]$Matches[1])) }
        }
    }
    return 100
}

function Set-Zoom([int]$Value) {
    New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
    Set-Content -LiteralPath $SettingsFile -Encoding ASCII -Value @(
        '[Thumbnails]',
        '; Magnification of the first page in percent: 100 shows the whole page, up to 200.',
        '; Above 100 the longer side is cropped evenly (top and bottom of portrait pages) so the',
        '; page fills more of the square thumbnail. Change it with: PdfThumbnails.cmd zoom 120',
        "Zoom=$Value")
}

# ---- state -----------------------------------------------------------------------------------

# The COM class registered for one scope ('user' or 'machine'), or $null.
function Get-ClassRegistration([string]$Scope) {
    $key = "$ClsidKey\InprocServer32"
    if ($Scope -eq 'machine') {
        $dll = Get-RegValue $HKLM $key
        $threading = Get-RegValue $HKLM $key 'ThreadingModel'
    } else {
        $dll = Get-UserValue $key
        $threading = if ($dll) { Get-UserValue $key 'ThreadingModel' } else { $null }
    }
    if (-not $dll) { return $null }
    $hasDll = Test-Path -LiteralPath $dll -PathType Leaf
    [pscustomobject]@{
        Dll       = $dll
        HasDll    = $hasDll
        HasPdfium = $hasDll -and (Test-Path -LiteralPath (Join-Path (Split-Path $dll) $PdfiumDll) -PathType Leaf)
        Threading = $threading
    }
}

function Get-State {
    $user = Get-ClassRegistration 'user'
    $machine = Get-ClassRegistration 'machine'
    [pscustomobject]@{
        # A machine-wide class means install -Machine was used; a per-user one is then a leftover.
        Scope    = if ($machine) { 'machine' } elseif ($user) { 'user' } else { $null }
        User     = $user
        Machine  = $machine
        Handlers = @($HandlerKeys | ForEach-Object { Get-UserValue $_ })
    }
}

function Test-ClassOk($Class) { [bool]($Class -and $Class.HasDll -and $Class.HasPdfium -and $Class.Threading -eq 'Apartment') }

function Test-Healthy($State) {
    $class = if ($State.Scope -eq 'machine') { $State.Machine } else { $State.User }
    (Test-ClassOk $class) -and -not ($State.Scope -eq 'machine' -and $State.User) -and
        @($State.Handlers | Where-Object { $_ -ne $Clsid }).Count -eq 0
}

function Get-SlotName([int]$Index) { $HandlerKeys[$Index] -replace '^Software\\Classes\\', '' -replace '\\ShellEx\\.*$', '' }

function Test-BuildFolder([string]$Dir) {
    (Test-Path -LiteralPath (Join-Path $Dir $HandlerDll)) -and (Test-Path -LiteralPath (Join-Path $Dir $PdfiumDll))
}

# The build next to this script: bin\ in the installed copy or in dist\PdfThumbnails, or the
# source tree's dist folder.
function Find-SourceBin {
    foreach ($dir in (Join-Path $PSScriptRoot 'bin'), (Join-Path $PSScriptRoot 'dist\PdfThumbnails\bin')) {
        if (Test-BuildFolder $dir) { return $dir }
    }
    return $null
}

# Newest complete app folder already installed per-user.
function Find-InstalledDll {
    $dirs = Get-ChildItem -LiteralPath (Join-Path $InstallRoot 'app') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-BuildFolder $_.FullName } | Sort-Object LastWriteTime -Descending
    foreach ($dir in $dirs) { return Join-Path $dir.FullName $HandlerDll }
    return $null
}

# ---- actions ---------------------------------------------------------------------------------

# Copies a build into <Root>\app\<version>-<hash>. Explorer's thumbnail process keeps the
# loaded DLLs open, so builds never overwrite each other in place.
function Install-Files([string]$SourceBin, [string]$Root) {
    $dll = Join-Path $SourceBin $HandlerDll
    $pdfium = Join-Path $SourceBin $PdfiumDll
    $version = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
    $hashes = (Get-FileHash -LiteralPath $dll, $pdfium -Algorithm SHA256 | ForEach-Object Hash) -join ''
    $sha = [Security.Cryptography.SHA256]::Create()
    $id = -join ($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($hashes))[0..3] | ForEach-Object { $_.ToString('x2') })
    $target = Join-Path $Root "app\$version-$id"
    if (-not (Test-BuildFolder $target)) {
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item -LiteralPath $dll, $pdfium -Destination $target -Force
    }
    return (Join-Path $target $HandlerDll)
}

# Deletes app folders other than $KeepDir; ones still loaded somewhere are kept for later.
function Remove-OldVersions([string]$Root, [string]$KeepDir, [string]$Later) {
    foreach ($dir in Get-ChildItem -LiteralPath (Join-Path $Root 'app') -Directory -ErrorAction SilentlyContinue) {
        if ($dir.FullName -eq $KeepDir) { continue }
        try { Remove-Item -LiteralPath $dir.FullName -Recurse -Force -ErrorAction Stop }
        catch { Write-Line "  kept $($dir.FullName) (still in use; $Later)" DarkGray }
    }
}

# The manager, its launcher, docs, licenses and a copy of the build live in the per-user
# install root, so everything (including install -Machine) also works from there.
function Copy-ManagerFiles([string]$Package) {
    New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
    if ((Resolve-Path -LiteralPath $Package).Path -eq $InstallRoot) { return }
    foreach ($name in 'PdfThumbnails.ps1', 'PdfThumbnails.cmd', 'README.md', 'LICENSE', 'licenses', 'bin') {
        $item = Join-Path $Package $name
        if (Test-Path -LiteralPath $item) { Copy-Item -LiteralPath $item -Destination $InstallRoot -Recurse -Force }
    }
}

function Register-UserClass([string]$Dll) {
    Set-UserValues $ClsidKey @{ '' = $ClassName }
    Set-UserValues "$ClsidKey\InprocServer32" @{ '' = $Dll; 'ThreadingModel' = 'Apartment' }
}

# Elevated step only.
function Set-MachineClass([string]$Dll) {
    $k = $HKLM.CreateSubKey($ClsidKey)
    try { $k.SetValue('', $ClassName) } finally { $k.Close() }
    $k = $HKLM.CreateSubKey("$ClsidKey\InprocServer32")
    try { $k.SetValue('', $Dll); $k.SetValue('ThreadingModel', 'Apartment') } finally { $k.Close() }
}

function Register-Association {
    foreach ($key in $HandlerKeys) { Set-UserValues $key @{ '' = $Clsid } }
    # A cleanup left pending by an earlier uninstall would delete this install at next sign-in.
    Remove-UserValue $RunOnceKey $CleanupValue
}

function Register-RepairTask {
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $script = Join-Path $InstallRoot 'PdfThumbnails.ps1'
    $powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    # conhost --headless runs the script without flashing a console window.
    $action = New-ScheduledTaskAction -Execute (Join-Path $env:WINDIR 'System32\conhost.exe') `
        -Argument "--headless `"$powershell`" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$script`" repair -Quiet"

    $atLogon = New-ScheduledTaskTrigger -AtLogOn -User $user
    $atLogon.Delay = 'PT1M'

    # Windows Installer logs event 1033 (product installed) for every PdfCraft install/update.
    $eventTrigger = Get-CimClass -Namespace 'Root/Microsoft/Windows/TaskScheduler' -ClassName 'MSFT_TaskEventTrigger'
    $afterPdfCraft = New-CimInstance -CimClass $eventTrigger -ClientOnly
    $afterPdfCraft.Enabled = $true
    $afterPdfCraft.Delay = 'PT30S'
    $afterPdfCraft.Subscription = '<QueryList><Query Id="0" Path="Application"><Select Path="Application">' +
        "*[System[Provider[@Name='MsiInstaller'] and EventID=1033] and EventData[Data='PdfCraft']]" +
        '</Select></Query></QueryList>'

    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $atLogon, $afterPdfCraft `
        -Principal $principal -Settings $settings -Force `
        -Description 'Re-checks the PdfThumbnails registration at logon and after PdfCraft installs or updates.' | Out-Null
}

function Get-RepairTask { Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue }

# Applies -AutoRepair / -NoAutoRepair.
function Update-RepairTask {
    if ($AutoRepair) { Register-RepairTask }
    if ($NoAutoRepair -and (Get-RepairTask)) { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false }
}

function Invoke-Repair {
    $state = Get-State
    $changed = $false
    $dll = $null
    if ($state.Scope -eq 'machine') {
        # The machine-wide class needs admin rights; everything per-user can be fixed here.
        if (-not (Test-ClassOk $state.Machine)) {
            Write-Line "ERROR: the machine-wide part is damaged. Run `"$InstallRoot\PdfThumbnails.cmd`" install -Machine (needs admin)." Red
            return $false
        }
        $dll = $state.Machine.Dll
        if ($state.User) { Remove-UserKey $ClsidKey; $changed = $true }  # it would shadow the machine class
        Remove-OldVersions $InstallRoot '' 'removed by the next repair'
    } else {
        if (Test-ClassOk $state.User) {
            $dll = $state.User.Dll
        } else {
            $dll = Find-InstalledDll
            if (-not $dll) {
                $source = Find-SourceBin
                if ($source) { $dll = Install-Files $source $InstallRoot }
            }
            if (-not $dll) {
                Write-Line "ERROR: the installed files are missing. Run install from the build's dist\PdfThumbnails folder." Red
                return $false
            }
            Register-UserClass $dll
            $changed = $true
        }
        Remove-OldVersions $InstallRoot (Split-Path $dll) 'removed by the next install or repair'
    }
    # Fill empty association slots, but leave another thumbnail handler alone: the user may
    # have chosen it. install takes the slots back.
    for ($i = 0; $i -lt $HandlerKeys.Count; $i++) {
        $value = $state.Handlers[$i]
        if (-not $value) {
            Set-UserValues $HandlerKeys[$i] @{ '' = $Clsid }
            $changed = $true
        } elseif ($value -ne $Clsid) {
            Write-Line "Left $(Get-SlotName $i) -> $value ($(Get-ClassName $value)): another thumbnail handler. Run install to take it back." Yellow
        }
    }
    if ($changed) {
        Remove-UserValue $RunOnceKey $CleanupValue
        Send-AssocChanged
        Write-Line "Repaired: .pdf thumbnails -> $dll" Green
    } else {
        Write-Line 'OK: registration intact.' Green
    }
    return $true
}

# Returns $true when everything was removed.
function Uninstall-All {
    # Only remove what is ours; other tools may have put their own handler in these slots.
    foreach ($key in $HandlerKeys) {
        if ((Get-UserValue $key) -eq $Clsid) { Remove-UserKey $key }
    }
    foreach ($key in 'Software\Classes\.pdf\ShellEx', 'Software\Classes\.pdf',
                     'Software\Classes\SystemFileAssociations\.pdf\ShellEx',
                     'Software\Classes\SystemFileAssociations\.pdf',
                     'Software\Classes\SystemFileAssociations') {
        Remove-UserKeyIfEmpty $key
    }
    Remove-UserKey $ClsidKey
    if (Get-RepairTask) { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false }
    $machineLeft = $false
    if ((Get-ClassRegistration 'machine') -or (Test-Path -LiteralPath $MachineRoot)) {
        [void](Invoke-Elevated 'machine-unregister')
        $machineLeft = [bool](Get-ClassRegistration 'machine') -or (Test-Path -LiteralPath $MachineRoot)
    }
    Send-AssocChanged

    if ($machineLeft) {
        # Keep the manager so the advice below still works.
        foreach ($name in 'app', 'repair.log', 'repair.log.old') {
            Remove-Item -LiteralPath (Join-Path $InstallRoot $name) -Recurse -Force -ErrorAction SilentlyContinue
        }
        Write-Line 'PDF thumbnails are off for you, but the machine-wide part (its COM class and C:\Program Files\PdfThumbnails) is still installed.' Yellow
        Write-Line "To remove it, run `"$InstallRoot\PdfThumbnails.cmd`" uninstall again and approve the administrator prompt." Yellow
        return $false
    }
    if (Test-Path -LiteralPath $InstallRoot) {
        # cmd.exe re-reads PdfThumbnails.cmd after PowerShell exits, so keep the running copy.
        $launcher = Join-Path $InstallRoot 'PdfThumbnails.cmd'
        if ($PSScriptRoot -eq $InstallRoot) {
            Get-ChildItem -LiteralPath $InstallRoot -Force | Where-Object { $_.FullName -ne $launcher } |
                Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        } else {
            Remove-Item -LiteralPath $InstallRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        # Whatever is left (the launcher, or DLLs Explorer still holds) goes at the next sign-in.
        if (Test-Path -LiteralPath $InstallRoot) {
            Set-UserValues $RunOnceKey @{ $CleanupValue = "cmd.exe /d /c rd /s /q `"$InstallRoot`"" }
        }
    }
    Write-Line 'PdfThumbnails removed. PDF files show their app icon again.' Green
    return $true
}

# Deletes Explorer's thumbnail cache so every thumbnail is extracted again (for example after
# changing the zoom). Explorer and its thumbnail processes hold the cache files open.
function Clear-ThumbnailCache {
    $cache = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Explorer'
    Write-Line 'Restarting Explorer to rebuild the thumbnail cache...' Yellow
    Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-CimInstance Win32_Process -Filter "Name='dllhost.exe'" |
        Where-Object { $_.CommandLine -match 'AB8902B4-09CA-4BB6-B78D-A8F59079A8D5' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    $inUse = 0
    foreach ($file in Get-ChildItem -LiteralPath $cache -Filter 'thumbcache_*.db' -Force) {
        try { Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop } catch { $inUse++ }
    }
    # Windows normally restarts Explorer by itself; make sure it is back.
    Start-Sleep -Seconds 3
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process (Join-Path $env:WINDIR 'explorer.exe') }
    if ($inUse) { Write-Line "  $inUse cache file(s) were in use by other programs; those thumbnails update after the next sign-in." DarkGray }
    Write-Line 'Thumbnail cache cleared; thumbnails are rebuilt as folders are opened.' Green
}

# How often Code Integrity refused to load the installed build's DLLs in the last 7 days
# (event 3077). Its messages use \Device\HarddiskVolumeN paths, so match the build folder name.
function Get-BlockCount([string]$Dll) {
    $pattern = '*\app\' + (Split-Path -Leaf (Split-Path $Dll)) + '\*'
    @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3077; StartTime = (Get-Date).AddDays(-7) } -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -like $pattern }).Count
}

# True for the HRESULT of a refused image load: 577 ERROR_INVALID_IMAGE_HASH, and 4551,
# 4556-4559, 4580-4583 ("An Application Control policy has blocked this file" and its
# reputation variants), which is how a Smart App Control block of PdfThumbnails.dll surfaces.
function Test-LoadBlockedError($Hr) {
    if ($Hr -isnot [int] -or ($Hr -band 0xFFFF0000) -ne 0x80070000) { return $false }
    return ($Hr -band 0xFFFF) -in 577, 4551, 4556, 4557, 4558, 4559, 4580, 4581, 4582, 4583
}

# What status says about Smart App Control, from its state (VerifiedAndReputablePolicyState:
# 0 off, 1 on, 2 evaluation), the Explorer test result ($null if it did not run) and logged
# blocks. Smart App Control does not block every unsigned DLL (for example ones built on this
# PC), so only evidence of a block counts: 'blocked', 'ok' (test passed), 'on' (no evidence
# either way), 'eval', or $null when it is off.
function Get-SmartAppControlVerdict($State, $Probe, [int]$Blocks) {
    if ($State -eq 2) { return 'eval' }
    if ($State -ne 1) { return $null }
    if ($Probe -eq 0) { return 'ok' }
    if ($Blocks -gt 0 -or (Test-LoadBlockedError $Probe)) { return 'blocked' }
    return 'on'
}

function Show-Status {
    $state = Get-State
    $healthy = Test-Healthy $state
    $mark = { param($ok) if ($ok) { 'OK ' } else { '!! ' } }
    $class = if ($state.Scope -eq 'machine') { $state.Machine } else { $state.User }

    Write-Line 'PdfThumbnails status' White
    $scopeText = switch ($state.Scope) { 'machine' { 'machine-wide COM class, per-user .pdf association' } 'user' { 'per-user' } default { 'not installed' } }
    Write-Line "     Scope         $scopeText"
    if ($class) {
        $version = if ($class.HasDll) { (Get-Item -LiteralPath $class.Dll).VersionInfo.FileVersion } else { 'missing' }
        Write-Line ("  {0}DLL           {1} ({2})" -f (& $mark $class.HasDll), $class.Dll, $version)
        Write-Line ("  {0}PDFium        {1}" -f (& $mark $class.HasPdfium), $(if ($class.HasPdfium) { "$PdfiumDll next to the DLL" } else { "$PdfiumDll missing" }))
    } else {
        Write-Line '  !! DLL           not registered (run: PdfThumbnails.cmd install)'
    }
    if ($state.Scope -eq 'machine' -and $state.User) {
        Write-Line "  !! A per-user COM class ($($state.User.Dll)) shadows the machine-wide one." Yellow
        Write-Line '                   Run install -Machine to update the machine-wide copy, or repair to drop the per-user one.' Yellow
    }
    for ($i = 0; $i -lt $HandlerKeys.Count; $i++) {
        $value = $state.Handlers[$i]
        $text = if (-not $value) { 'none' } elseif ($value -eq $Clsid) { $value } else { "$value ($(Get-ClassName $value), another handler)" }
        Write-Line ("  {0}Association   {1} -> {2}" -f (& $mark ($value -eq $Clsid)), (Get-SlotName $i), $text)
    }

    # This process may see a private copy of HKCU (inside an MSIX-packaged app); the checks
    # that go through Windows' association code would then describe that copy, not Explorer.
    $probe = $null  # HRESULT of the Explorer test, if it runs
    $ownView = Get-RegValue ([Microsoft.Win32.Registry]::CurrentUser) $HandlerKeys[0]
    if ($ownView -ne $state.Handlers[0]) {
        Write-Line '     Note          this terminal sees a private copy of your registry (it runs inside a packaged app),'
        Write-Line '                   so the Windows checks are skipped; double-click PdfThumbnails.cmd to run them.'
    } elseif ($Native) {
        $effective = Get-EffectiveHandler
        $progId = [PdfThumbsShell]::Query(20, '.pdf', $null)
        if ($effective -eq $Clsid) {
            Write-Line '  OK Effective     Windows resolves .pdf thumbnails to PdfThumbnails'
        } else {
            Write-Line ("  !! Effective     Windows resolves .pdf thumbnails to {0}" -f $(if ($effective) { "$effective ($(Get-ClassName $effective))" } else { 'nothing' }))
            $healthy = $false
        }
        if ($progId) {
            # A handler registered on the default app's own ProgID outranks the .pdf slots.
            $appHandler = Get-ClassesValue "$progId\ShellEx\$ThumbnailKey"
            if ($appHandler -and $appHandler -ne $Clsid) {
                Write-Line "  !! Default app   $progId brings its own thumbnail handler $appHandler, which takes precedence" Yellow
            }
            $overlay = Get-ClassesValue $progId 'TypeOverlay'
            $overlayText = if ($null -eq $overlay) { 'its icon is drawn on thumbnails' } elseif ($overlay -eq '') { 'no icon overlay' } else { "overlay $overlay" }
            Write-Line "     Default app   $progId ($overlayText)"
        }
        if ($effective -eq $Clsid) {
            if ([PdfThumbsShell]::ElevationType() -eq 2 -and $state.Scope -eq 'user') {
                # Elevated processes never load per-user COM classes, but Explorer is not elevated.
                Write-Line '     Explorer test skipped: this prompt is elevated; run status from a normal prompt'
            } else {
                $hr = Test-ExplorerExtraction
                $probe = $hr
                if ($hr -eq 0) {
                    Write-Line "  OK Explorer test a thumbnail was extracted in Explorer's isolated thumbnail process"
                } elseif ($hr -eq -2147221164 -and $state.Scope -eq 'user') {  # 0x80040154 REGDB_E_CLASSNOTREG
                    Write-Line "  !! Explorer test 0x80040154: Explorer's thumbnail process cannot load the per-user COM class;" Yellow
                    Write-Line '                   run "PdfThumbnails.cmd install -Machine" (one administrator prompt)' Yellow
                    $healthy = $false
                } elseif ($hr -eq -2147221164 -and -not $state.Scope) {
                    Write-Line '  !! Explorer test 0x80040154: the COM class is not registered; run install, or uninstall to clean up'
                    $healthy = $false
                } else {
                    Write-Line ("  !! Explorer test extraction failed: 0x{0:X8}" -f $hr)
                    $healthy = $false
                }
            }
        }
    }

    Write-Line "     Zoom          $(Get-Zoom)% (change with: PdfThumbnails.cmd zoom <percent>)"
    if ((Get-UserDword 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' 'IconsOnly') -eq 1) {
        Write-Line '  !! Explorer      "Always show icons, never thumbnails" is on (Folder Options > View)'
    }
    $sac = Get-RegValue $HKLM 'SYSTEM\CurrentControlSet\Control\CI\Policy' 'VerifiedAndReputablePolicyState'
    $blocks = 0
    if ($sac -eq 1 -and $class -and $probe -ne 0) {
        $blocks = Get-BlockCount $class.Dll
        # A block caused by the test itself (for example of the PDFium DLL, which the test only
        # sees as a failed extraction) reaches the log within about a second.
        if (-not $blocks -and $null -ne $probe -and -not (Test-LoadBlockedError $probe)) {
            Start-Sleep -Milliseconds 1500
            $blocks = Get-BlockCount $class.Dll
        }
    }
    switch (Get-SmartAppControlVerdict $sac $probe $blocks) {
        'eval' { Write-Line '     Smart App Control is in evaluation mode; if Windows switches it ON, it may block these unsigned DLLs.' }
        'ok'   { Write-Line '     Smart App Control is ON and is not blocking PdfThumbnails (the Explorer test passed)' }
        'on'   {
            if ($class) { Write-Line '     Smart App Control is ON; Code Integrity logged no blocks of the installed DLLs in the last 7 days' }
            else { Write-Line '     Smart App Control is ON' }
        }
        'blocked' {
            $evidence = if ($blocks) { "Code Integrity blocked the installed DLLs $blocks time(s) in the last 7 days" } else { "Windows refused to load the DLL, 0x{0:X8}" -f $probe }
            Write-Line "  !! Smart App Control is ON and is blocking PdfThumbnails ($evidence). Turn it off, or run" Yellow
            Write-Line '                   "PdfThumbnails.cmd uninstall" and enable the PDF thumbnails in Microsoft PowerToys.' Yellow
            $healthy = $false
        }
    }
    $task = Get-RepairTask
    Write-Line ("     Auto-repair   {0}" -f $(if ($task) { 'on (at sign-in and after PdfCraft installs/updates)' } else { 'off' }))

    if ($TestFile) {
        if (-not $Native) {
            Write-Line '  !! Test file     unavailable (C# helpers could not be compiled)'
        } else {
            try {
                $path = Convert-Path -LiteralPath $TestFile
                $width = 0
                $height = 0
                $watch = [Diagnostics.Stopwatch]::StartNew()
                $hr = [PdfThumbsShell]::ProbeExplorer($path, 256, [ref]$width, [ref]$height)
                if ($hr -eq 0) {
                    Write-Line ("  OK Test file     {0}x{1} thumbnail extracted in {2} ms" -f $width, $height, $watch.ElapsedMilliseconds)
                } else {
                    Write-Line ("  !! Test file     no thumbnail: 0x{0:X8}" -f $hr)
                    $healthy = $false
                }
            } catch {
                Write-Line "  !! Test file     $(Get-ErrorText $_)"
                $healthy = $false
            }
        }
    }
    return $healthy
}

# ---- main ------------------------------------------------------------------------------------

switch ($Command) {
    'status' {
        if (Show-Status) { exit 0 } else { exit 1 }
    }
    'install' {
        if (-not $Machine -and (Get-ClassRegistration 'machine')) {
            Write-Line 'PdfThumbnails is installed machine-wide, so this updates the machine-wide copy (one administrator prompt).' Yellow
            Write-Line 'To switch to a per-user install, run uninstall first.' Yellow
            $Machine = $true
        }
        $source = Find-SourceBin
        if (-not $source) { throw "No build found in $PSScriptRoot\bin. Run install from a build's dist\PdfThumbnails folder (build.cmd makes one)." }
        if ($Native -and [PdfThumbsShell]::ElevationType() -eq 2) {
            Write-Line 'Note: this is an administrator prompt. A normal one is enough; -Machine asks for admin rights itself.' DarkGray
        }
        Copy-ManagerFiles (Split-Path $source)
        if ($Machine) {
            # The local copy made above, so the elevated step never depends on the build folder.
            if ((Invoke-Elevated 'machine-register' @('-Source', (Join-Path $InstallRoot 'bin'))) -ne 0) {
                throw 'The machine-wide registration did not complete (see the messages above).'
            }
            Remove-UserKey $ClsidKey  # a per-user class would shadow the machine-wide one
            Remove-OldVersions $InstallRoot '' 'removed by the next repair'
        } else {
            $dll = Install-Files $source $InstallRoot
            Register-UserClass $dll
            Remove-OldVersions $InstallRoot (Split-Path $dll) 'removed by the next install or repair'
        }
        Register-Association
        if (-not (Test-Path -LiteralPath $SettingsFile)) { Set-Zoom 100 }
        Send-AssocChanged
        Update-RepairTask
        Write-Line 'Installed.' Green
        if (Show-Status) { exit 0 } else { exit 1 }
    }
    'repair' {
        $ok = Invoke-Repair
        Update-RepairTask
        $effective = Get-EffectiveHandler
        Write-Line ("  Windows resolves .pdf thumbnails to {0}" -f $(if ($effective -eq $Clsid) { 'PdfThumbnails' } elseif ($effective) { $effective } else { 'nothing' }))
        if ($ok) { exit 0 } else { exit 1 }
    }
    'zoom' {
        if (-not $PSBoundParameters.ContainsKey('Percent')) {
            Write-Line "Zoom: $(Get-Zoom)%"
            exit 0
        }
        Set-Zoom $Percent
        Send-AssocChanged
        Write-Line "Zoom set to $Percent%. Thumbnails extracted from now on use it; to redo the ones Explorer" Green
        Write-Line 'has already cached, run: PdfThumbnails.cmd refresh (restarts Explorer)' Green
        exit 0
    }
    'refresh' {
        Clear-ThumbnailCache
        exit 0
    }
    'uninstall' {
        if (Uninstall-All) { exit 0 } else { exit 1 }
    }
    'machine-register' {
        try {
            if (-not (Test-Admin)) { throw 'machine-register must run as administrator.' }
            if (-not $Source -or -not (Test-BuildFolder $Source)) { throw "machine-register needs -Source <folder with $HandlerDll and $PdfiumDll>." }
            $dll = Install-Files $Source $MachineRoot
            Set-MachineClass $dll
            Remove-OldVersions $MachineRoot (Split-Path $dll) 'removed by the next install -Machine or uninstall'
            Write-Line "Machine-wide COM class -> $dll"
            exit 0
        } catch {
            Write-Line "ERROR: $(Get-ErrorText $_)" Red
            exit 1
        }
    }
    'machine-unregister' {
        try {
            if (-not (Test-Admin)) { throw 'machine-unregister must run as administrator.' }
            $HKLM.DeleteSubKeyTree($ClsidKey, $false)
            if (Test-Path -LiteralPath $MachineRoot) {
                Remove-Item -LiteralPath $MachineRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
            if (Test-Path -LiteralPath $MachineRoot) {
                # DLLs still loaded by a thumbnail process cannot be deleted, but they can be moved.
                # Park them outside the install folder (same volume) and delete them at the next
                # restart, so a reinstall before then never reuses a path queued for deletion.
                $trash = Join-Path (Split-Path $MachineRoot) ('PdfThumbnails.trash-' + [guid]::NewGuid().ToString('N'))
                New-Item -ItemType Directory -Path $trash | Out-Null
                foreach ($file in @(Get-ChildItem -LiteralPath $MachineRoot -Recurse -File -Force)) {
                    Move-Item -LiteralPath $file.FullName -Destination (Join-Path $trash ([guid]::NewGuid().ToString('N') + $file.Extension))
                }
                Remove-Item -LiteralPath $MachineRoot -Recurse -Force
                if ($Native) {
                    foreach ($file in @(Get-ChildItem -LiteralPath $trash -File -Force)) { [void][PdfThumbsShell]::DeleteAtRestart($file.FullName) }
                    [void][PdfThumbsShell]::DeleteAtRestart($trash)
                    Write-Line 'DLLs still in use were moved aside; Windows deletes them at the next restart.'
                } else {
                    Write-Line "DLLs still in use were moved to $trash; delete that folder after a restart."
                }
            }
            exit 0
        } catch {
            Write-Line "ERROR: $(Get-ErrorText $_)" Red
            exit 1
        }
    }
}
