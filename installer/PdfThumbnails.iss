; The installer: one setup for x64 and ARM64 PCs that installs for all users.
;
; Build it with cmake/Installer.cmake, which passes every name, version and path this script uses
; as /D values (docs/BUILDING.md, "The installer"); the script holds none of them itself.
;
; What it does:
; - Files: the two executables and the notices go into <Program Files>\<AppName>, the two DLLs
;   into app\<build folder>, one folder per build, named after the DLLs' bytes
;   (cmake/BuildId.cmake). A DLL already there is the same build, and File Explorer's thumbnail
;   process may have it loaded, so it is never replaced (onlyifdoesntexist). Nothing is ever
;   queued for deletion at a restart: Windows would delete a reinstalled build at boot.
; - The registry: none of the product's values are written here. After the files, the settings
;   program's own steps run: --setup-machine --build <folder> as administrator (the class for all
;   users, the task folder, what 1.x and older builds left), then --setup-user as the user who
;   started Setup (the .pdf settings, the zoom, the check task, 1.x's per-user leftovers).
; - Uninstall: --uninstall-cleanup runs first, as administrator. It removes the product for every
;   signed-in user and everything in the install folder except the uninstaller's own files, which
;   the uninstaller then deletes with the folder.
; - No AppMutex: a scheduled check holds the app's names for up to 30 seconds while it waits for
;   another one, which would stop Setup and the uninstaller on a question even when silent.
;   Running programs in the install folder are closed through the Restart Manager instead
;   (*.exe only: never File Explorer's thumbnail process, which holds the DLLs).
; - Exit codes: Inno Setup's own; otherwise 100 + the exit code of --setup-machine when it failed,
;   or 200 + that of --setup-user (208: Setup was started as administrator, so the signed-in
;   user's part is left to them: PdfThumbnailsCli.exe --setup-user, run as themselves).

#ifndef AppName
  #error Build this script with cmake/Installer.cmake, which passes the values it needs.
#endif

[Setup]
AppId={{{#AppIdGuid}}
AppName={#AppName}
AppVersion={#VersionText}
AppVerName={#AppName} {#VersionText}
AppPublisher={#Publisher}
AppPublisherURL={#Homepage}
AppSupportURL={#Homepage}/issues
AppUpdatesURL={#Homepage}/releases
AppCopyright={#Copyright}
VersionInfoVersion={#FileVersion}
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#FileVersion}
VersionInfoProductTextVersion={#VersionText}
VersionInfoDescription={#AppName} Setup
VersionInfoCompany={#Publisher}
VersionInfoCopyright={#Copyright}
; The settings program's administrator steps run only from <Program Files>\<AppName>.
DefaultDirName={autopf}\{#AppName}
DisableDirPage=yes
UsePreviousAppDir=no
DisableProgramGroupPage=yes
; For all users only: on current Windows 11 builds the thumbnail process loads only what is
; installed for all users.
PrivilegesRequired=admin
ArchitecturesAllowed=x64os or arm64
ArchitecturesInstallIn64BitMode=x64os or arm64
MinVersion=10.0.17763
SetupMutex={#AppName}Setup,Global\{#AppName}Setup
CloseApplications=yes
CloseApplicationsFilter=*.exe
RestartApplications=no
AlwaysRestart=no
RestartIfNeededByRun=no
ChangesAssociations=no
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#SettingsExe}
SetupLogging=yes
SetupIconFile={#SetupIcon}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}

[Dirs]
; The install folder may be there before Setup (a developer install, 1.x): the uninstaller then
; still removes it once it is empty.
Name: "{app}"; Flags: uninsalwaysuninstall

[Files]
Source: "{#RuntimeX64}\{#SettingsExe}"; DestDir: "{app}"; Flags: ignoreversion; Check: not IsArm64
Source: "{#RuntimeX64}\{#CliExe}"; DestDir: "{app}"; Flags: ignoreversion; Check: not IsArm64
Source: "{#RuntimeX64}\{#HandlerDll}"; DestDir: "{app}\app\{#BuildX64}"; Flags: onlyifdoesntexist; Check: not IsArm64
Source: "{#RuntimeX64}\{#PdfiumDll}"; DestDir: "{app}\app\{#BuildX64}"; Flags: onlyifdoesntexist; Check: not IsArm64
Source: "{#RuntimeArm64}\{#SettingsExe}"; DestDir: "{app}"; Flags: ignoreversion; Check: IsArm64
Source: "{#RuntimeArm64}\{#CliExe}"; DestDir: "{app}"; Flags: ignoreversion; Check: IsArm64
Source: "{#RuntimeArm64}\{#HandlerDll}"; DestDir: "{app}\app\{#BuildArm64}"; Flags: onlyifdoesntexist; Check: IsArm64
Source: "{#RuntimeArm64}\{#PdfiumDll}"; DestDir: "{app}\app\{#BuildArm64}"; Flags: onlyifdoesntexist; Check: IsArm64
Source: "{#RuntimeX64}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RuntimeX64}\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RuntimeX64}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#SettingsExe}"; WorkingDir: "{sys}"; Comment: "{#Description}"

[Run]
Filename: "{app}\{#SettingsExe}"; Description: "Open {#AppName} settings"; WorkingDir: "{sys}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{app}\{#SettingsExe}"; Parameters: "--uninstall-cleanup"; WorkingDir: "{sys}"; Flags: runhidden waituntilterminated; RunOnceId: "Cleanup"

[Code]
var
  CustomExitCode: Integer;

// Runs one of the settings program's steps from the system folder, so no folder of the install
// is a step's current folder, and returns its exit code (99 when it could not start or its code
// is out of range). The administrator step's output goes to Setup's log.
function RunStep(const Params: String; AsOriginalUser: Boolean): Integer;
var
  Exe: String;
  Started: Boolean;
  ResultCode: Integer;
begin
  Exe := ExpandConstant('{app}\{#SettingsExe}');
  Log('Running ' + Exe + ' ' + Params);
  if AsOriginalUser then
    Started := ExecAsOriginalUser(Exe, Params, ExpandConstant('{sys}'), SW_HIDE,
      ewWaitUntilTerminated, ResultCode)
  else
    Started := ExecAndLogOutput(Exe, Params, ExpandConstant('{sys}'), SW_HIDE,
      ewWaitUntilTerminated, ResultCode, nil);
  if not Started then begin
    Log('It could not start: ' + SysErrorMessage(ResultCode));
    Result := 99;
  end else begin
    Log(Format('Exit code %d', [ResultCode]));
    Result := ResultCode;
    if (Result < 0) or (Result > 99) then
      Result := 99;
  end;
end;

// The install folder is fixed: /DIR= on the command line is refused.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Expected: String;
begin
  Result := '';
  Expected := ExpandConstant('{autopf}\{#AppName}');
  if CompareText(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), Expected) <> 0 then
    Result := '{#AppName} installs only into ' + Expected + '.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Build: String;
  Code: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;
  if IsArm64 then
    Build := '{#BuildArm64}'
  else
    Build := '{#BuildX64}';
  // 1: files wait for a restart (an old build still in use); that is fine.
  Code := RunStep('--setup-machine --build ' + Build, False);
  if (Code <> 0) and (Code <> 1) then begin
    CustomExitCode := 100 + Code;
    SuppressibleMsgBox('{#AppName} could not finish setting up this PC (exit code ' +
      IntToStr(Code) + '). Setup''s log has the details. Open {#AppName} from the Start menu ' +
      'to see what is missing.', mbError, MB_OK, IDOK);
    Exit;
  end;
  Code := RunStep('--setup-user', True);
  if Code = 8 then begin
    CustomExitCode := 200 + Code;
    SuppressibleMsgBox('Setup was started as administrator, so it could not set up {#AppName} ' +
      'for your own account. To finish, open a Command Prompt as yourself (not as administrator) ' +
      'and run:' + #13#10#13#10 + '"' + ExpandConstant('{app}\{#CliExe}') + '" --setup-user',
      mbInformation, MB_OK, IDOK);
  end else if Code <> 0 then begin
    CustomExitCode := 200 + Code;
    SuppressibleMsgBox('{#AppName} is installed, but it could not finish setting up your account ' +
      '(exit code ' + IntToStr(Code) + '). Open {#AppName} from the Start menu to see what is ' +
      'missing.', mbError, MB_OK, IDOK);
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := CustomExitCode;
end;
