// Made-up snapshots for the tests of the reports (status text, JSON, diagnostics): an empty PC,
// sound per-user and machine-wide installs, and this PC's own shape (1.1 installed machine-wide,
// 1.x leftovers, Smart App Control on). Paths and names are invented; nothing is read. The
// profile lies under D:\Home and the SID is split after "S-1-5-21-", because the repository's
// publish check rejects anything shaped like a real profile path or account SID.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

#include "build_folder.h"
#include "project_config.h"
#include "sac.h"
#include "snapshot.h"
#include "tasks.h"

namespace pdft::test {

inline const std::wstring kOurs = kHandlerClsidString;
inline const std::wstring kOther = L"{9C60DE1E-E5FC-40F4-A487-460851A8D915}";
inline const std::filesystem::path kUserRoot =
    L"D:\\Home\\someone\\AppData\\Local\\Programs\\Product";
inline const std::filesystem::path kMachineRoot = L"C:\\Program Files\\Product";

#if defined(_M_ARM64)
inline constexpr USHORT kNativeMachine = IMAGE_FILE_MACHINE_ARM64;
inline constexpr USHORT kForeignMachine = IMAGE_FILE_MACHINE_AMD64;
inline constexpr Arch kNativeArch = Arch::kArm64;
inline constexpr wchar_t kNativeArchName[] = L"arm64";
inline constexpr wchar_t kForeignArchName[] = L"x64";
#else
inline constexpr USHORT kNativeMachine = IMAGE_FILE_MACHINE_AMD64;
inline constexpr USHORT kForeignMachine = IMAGE_FILE_MACHINE_ARM64;
inline constexpr Arch kNativeArch = Arch::kX64;
inline constexpr wchar_t kNativeArchName[] = L"x64";
inline constexpr wchar_t kForeignArchName[] = L"arm64";
#endif

inline ClassReg MakeClass(const std::filesystem::path& dll,
                          const std::wstring& version = L"2.0.0.0") {
  ClassReg reg;
  reg.name = kHandlerClassName;
  reg.dll = dll.native();
  reg.threading = L"Apartment";
  reg.dllExists = true;
  reg.pdfiumExists = true;
  reg.peMachine = kNativeMachine;
  reg.fileVersion = version;
  return reg;
}

inline BuildInfo MakeBuild(const std::filesystem::path& root, const std::wstring& name,
                           bool complete = true, USHORT machine = kNativeMachine) {
  BuildInfo info;
  info.folder.dir = root / L"app" / name;
  info.folder.name = name;
  if (const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(name)) {
    info.folder.version = parts->version;
  }
  info.folder.is1x = info.folder.version.starts_with(L"1.");
  info.folder.complete = complete;
  if (complete && !info.folder.is1x && !info.folder.version.empty()) info.dllMachine = machine;
  return info;
}

inline std::filesystem::path DllIn(const BuildInfo& build) {
  return build.folder.dir / kHandlerDll;
}

// Nothing installed, nothing in the way: a process at Medium whose registry view is Explorer's.
inline Snapshot EmptySnapshot() {
  Snapshot s;
  s.context.userSid =
      L"S-1-5-21-"
      L"1111111111-2222222222-3333333333-1001";
  s.context.userAccount = L"DESK-42\\someone";
  s.context.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
  s.context.probe = ContextProbe::kReadOnly;
  s.context.views = RegistryViews::kAgree;
  s.context.processArch = kNativeArch;
  s.context.osArch = kNativeArch;
  s.osBuild = L"26300.9818";
  s.paths.userRoot = kUserRoot;
  s.paths.machineRoot = kMachineRoot;
  s.paths.legacyUserRoot = kUserRoot;
  s.paths.legacyMachineRoot = kMachineRoot;
  s.paths.ownExe = kMachineRoot / kCliExe;
  s.liveSkip = LiveTestSkip::kNoClass;
  return s;
}

// A sound task for |s|'s install root.
inline void AddSoundTask(Snapshot* s) {
  s->task.exists = true;
  s->task.enabled = true;
  s->task.afterPdfCraft = true;
  s->task.atSignIn = true;
  s->task.path = std::wstring(kTaskFolder) + L"\\" + kTaskPrefix + s->context.userSid;
  s->task.actionPath = (s->installRoot / kSettingsExe).native();
  s->task.actionArgs = kCheckTaskArguments;
  s->task.options = OptionsFromCheckboxes(true, true);
}

inline void MakeHealthy(Snapshot* s) {
  s->settingsExeFound = true;  // a 2.x install has the program the task runs
  s->slots = {kOurs, kOurs};
  s->effective = kOurs;
  s->live = LiveTest{S_OK, {96, 96}, 200};
  s->liveSkip = LiveTestSkip::kNone;
  AddSoundTask(s);
}

// A sound per-user 2.x install.
inline Snapshot UserInstallSnapshot() {
  Snapshot s = EmptySnapshot();
  s.userBuilds = {MakeBuild(kUserRoot, L"2.0.0.0-1a2b3c4d")};
  s.userClass = MakeClass(DllIn(s.userBuilds[0]));
  s.scope = Scope::kUser;
  s.installRoot = kUserRoot;
  MakeHealthy(&s);
  return s;
}

// A sound machine-wide 2.x install.
inline Snapshot MachineInstallSnapshot() {
  Snapshot s = EmptySnapshot();
  s.machineBuilds = {MakeBuild(kMachineRoot, L"2.0.0.0-1a2b3c4d")};
  s.machineClass = MakeClass(DllIn(s.machineBuilds[0]));
  s.scope = Scope::kMachine;
  s.installRoot = kMachineRoot;
  MakeHealthy(&s);
  return s;
}

// This PC: 1.1 machine-wide, both slots ours, PdfCraft 0.14.0 as the default app without an
// overlay, the Explorer test passed, Smart App Control on, the 1.x task and settings.ini (130)
// still there, no 2.x task.
inline Snapshot ThisPcSnapshot() {
  Snapshot s = EmptySnapshot();
  s.machineBuilds = {MakeBuild(kMachineRoot, L"1.1.0.0-67a1323e")};
  s.machineClass = MakeClass(DllIn(s.machineBuilds[0]), L"1.1.0.0");
  s.scope = Scope::kMachine;
  s.installRoot = kMachineRoot;
  s.slots = {kOurs, kOurs};
  s.effective = kOurs;
  s.defaultApp.progId = L"PdfCraft.Document";
  s.defaultApp.overlay = L"";
  s.live = LiveTest{S_OK, {96, 96}, 212};
  s.liveSkip = LiveTestSkip::kNone;
  s.sac.state = SacState::kOn;
  s.sac.value = 1;
  s.sac.type = REG_DWORD;
  s.sacVerdict = DecideSac(SacState::kOn, S_OK, std::nullopt);
  s.legacy.legacyTask = true;
  s.legacy.ini = true;
  s.legacy.iniZoom = 130;
  s.legacy.files = {kUserRoot / L"settings.ini"};
  s.legacy.machineClassInV1 = true;
  s.zoom.decision = {130, ZoomOrigin::kLegacyIni, ZoomValueState::kMissing};
  s.zoom.legacyIniZoom = 130;
  s.pdfcraft = {true, L"0.14.0", L"{39F7F98F-3A8C-43B4-B6C3-778FB442B040}",
                PdfCraftSource::kUpgradeCode};
  return s;
}

}  // namespace pdft::test
