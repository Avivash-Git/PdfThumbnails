// The install and uninstall side's pure parts: the files of a build, the versions it installs,
// how the administrator step's outcome becomes --install's exit code, the class path the elevated
// side may write for all users, the user hives it cleans, the per-user files the uninstall
// removes, the trash folders the status reports, and the texts of --install, --uninstall and
// --zoom.

#include <windows.h>

#include "install.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "commands.h"
#include "file_ops.h"
#include "project_config.h"
#include "registration.h"
#include "snapshot_fixtures.h"
#include "uninstall.h"

namespace pdft {
namespace {

using std::filesystem::path;

TEST(Install, BuildFiles) {
  const std::span<const std::wstring_view> binaries = BuildBinaries();
  ASSERT_EQ(binaries.size(), 4u);
  EXPECT_EQ(binaries[0], kSettingsExe);
  EXPECT_EQ(binaries[1], kCliExe);
  EXPECT_EQ(binaries[2], kHandlerDll);
  EXPECT_EQ(binaries[3], kPdfiumDll);
  const std::span<const std::wstring_view> notices = BuildTopNotices();
  ASSERT_EQ(notices.size(), 2u);
  EXPECT_EQ(notices[0], L"LICENSE.txt");
  EXPECT_EQ(notices[1], L"THIRD-PARTY-NOTICES.md");
}

TEST(Install, InstallableVersions) {
  EXPECT_TRUE(IsInstallableVersion(L"2.0.0.0"));
  EXPECT_TRUE(IsInstallableVersion(L"2"));
  EXPECT_TRUE(IsInstallableVersion(L"10.1"));
  EXPECT_FALSE(IsInstallableVersion(L"1.1.0.0"));
  EXPECT_FALSE(IsInstallableVersion(L"1.99.99.99"));
  EXPECT_FALSE(IsInstallableVersion(L"0.9"));
  EXPECT_FALSE(IsInstallableVersion(L""));
  EXPECT_FALSE(IsInstallableVersion(L"2.0\\..\\x"));
  EXPECT_FALSE(IsInstallableVersion(L"2.0.0.0.0"));
}

TEST(Install, StepExitCodes) {
  EXPECT_FALSE(InstallStepExitCode(ElevatedOutcome::kDone, S_OK));
  // A restart pending is no exit code of its own: the status reports it.
  EXPECT_FALSE(InstallStepExitCode(ElevatedOutcome::kRestartPending, S_OK));
  EXPECT_EQ(InstallStepExitCode(ElevatedOutcome::kCancelled, S_OK), ExitCode::kCancelled);
  EXPECT_EQ(InstallStepExitCode(ElevatedOutcome::kFailed, kUnsafeBuildFolderHr),
            ExitCode::kWrongContext);
  EXPECT_EQ(InstallStepExitCode(ElevatedOutcome::kFailed, E_ACCESSDENIED), ExitCode::kFailed);
  EXPECT_EQ(InstallStepExitCode(ElevatedOutcome::kFailed, E_UNEXPECTED), ExitCode::kFailed);
}

TEST(Install, MachineClassPath) {
  const path root = L"C:\\Program Files\\Product";
  const path dll = root / L"app" / L"2.0.0.0-1a2b3c4d" / kHandlerDll;
  EXPECT_TRUE(IsMachineClassPath(root, dll));
  EXPECT_TRUE(IsMachineClassPath(L"C:\\Program Files\\Product\\", dll));
  EXPECT_TRUE(IsMachineClassPath(root, root / L"APP" / L"10.0-00000000" / kHandlerDll));
  // Never a 1.x build, never anything but the handler, nowhere but <root>\app\<build>.
  EXPECT_FALSE(IsMachineClassPath(root, root / L"app" / L"1.1.0.0-67a1323e" / kHandlerDll));
  EXPECT_FALSE(IsMachineClassPath(root, root / L"app" / L"2.0.0.0-1a2b3c4d" / kPdfiumDll));
  EXPECT_FALSE(IsMachineClassPath(root, root / L"app" / L"2.0.0.0" / kHandlerDll));
  EXPECT_FALSE(IsMachineClassPath(root, root / L"app" / L"x" / L"2.0.0.0-1a2b3c4d" / kHandlerDll));
  EXPECT_FALSE(IsMachineClassPath(root, root / L"bin" / L"2.0.0.0-1a2b3c4d" / kHandlerDll));
  EXPECT_FALSE(
      IsMachineClassPath(root, root / L"app" / L".." / L"app" / L"2.0.0.0-1a2b3c4d" / kHandlerDll));
  EXPECT_FALSE(IsMachineClassPath(
      root, L"D:\\Home\\someone\\Product\\app\\2.0.0.0-1a2b3c4d\\" + std::wstring(kHandlerDll)));
  EXPECT_FALSE(IsMachineClassPath(root, path(L"app\\2.0.0.0-1a2b3c4d") / kHandlerDll));
  EXPECT_FALSE(IsMachineClassPath(L"relative", path(L"relative\\app\\2.0-1a2b3c4d") / kHandlerDll));
}

TEST(Install, TrashFolderNames) {
  const std::wstring prefix = std::wstring(kProductName) + L".trash-";
  EXPECT_TRUE(IsTrashFolderName(prefix + L"0123456789abcdef0123456789ABCDEF"));
  EXPECT_FALSE(IsTrashFolderName(prefix + L"0123456789abcdef0123456789abcde"));
  EXPECT_FALSE(IsTrashFolderName(prefix + L"0123456789abcdef0123456789abcdefg"));
  EXPECT_FALSE(IsTrashFolderName(prefix + L"0123456789abcdef0123456789abcdeg"));
  EXPECT_FALSE(IsTrashFolderName(L"Other.trash-0123456789abcdef0123456789abcdef"));
  EXPECT_FALSE(IsTrashFolderName(kProductName));
}

TEST(Uninstall, UserHiveNames) {
  bool classes = true;
  EXPECT_TRUE(
      IsUserHiveName(L"S-1-5-21-"
                     L"1-2-3-1001",
                     &classes));
  EXPECT_FALSE(classes);
  EXPECT_TRUE(
      IsUserHiveName(L"S-1-5-21-"
                     L"1-2-3-1001_Classes",
                     &classes));
  EXPECT_TRUE(classes);
  EXPECT_TRUE(
      IsUserHiveName(L"S-1-5-21-"
                     L"1-2-3-1001_classes"));
  for (const wchar_t* name :
       {L"S-1-5-18", L"S-1-5-19", L"S-1-5-20", L".DEFAULT", L"S-1-5-21-", L"S-1-5-21",
        L"S-1-5-21--1", L"S-1-5-21-1-", L"S-1-5-21-1--2", L"S-1-5-21-1a", L"S-1-5-21-1_Classes_x",
        L"S-1-5-21-_Classes", L"s-1-5-21-1-2", L"S-1-12-1-1-2-3-4", L""}) {
    EXPECT_FALSE(IsUserHiveName(name, &classes)) << ::testing::PrintToString(std::wstring(name));
    EXPECT_FALSE(classes);
  }
}

TEST(Uninstall, UserRootFiles) {
  const std::span<const std::wstring_view> files = UserRootFiles();
  ASSERT_FALSE(files.empty());
  EXPECT_EQ(files[0], kSettingsExe);
  EXPECT_EQ(files[1], kCliExe);
  // The licenses folder comes after everything in it; app\ goes as a whole, and nothing of
  // 1.x's own names (its manager, README.md, LICENSE, bin\) is in the list.
  EXPECT_EQ(files.back(), L"licenses");
  for (size_t i = 0; i + 1 < files.size(); ++i) {
    EXPECT_NE(files[i], L"licenses");
    EXPECT_NE(files[i], L"app");
  }
  for (const std::wstring_view file : files) {
    for (const wchar_t* v1 : {L"README.md", L"LICENSE", L"bin", L"settings.ini"}) {
      EXPECT_NE(file, v1);
    }
    EXPECT_NE(file, std::wstring(kLegacyRootName) + L".ps1");
    EXPECT_NE(file, std::wstring(kLegacyRootName) + L".cmd");
  }
}

TEST(Uninstall, AnythingToUninstall) {
  using test::EmptySnapshot;
  Snapshot s = EmptySnapshot();
  EXPECT_FALSE(AnythingToUninstall(s, false, false, false, false));
  EXPECT_TRUE(AnythingToUninstall(s, true, false, false, false));
  EXPECT_TRUE(AnythingToUninstall(s, false, true, false, false));
  EXPECT_TRUE(AnythingToUninstall(s, false, false, true, false));
  EXPECT_TRUE(AnythingToUninstall(s, false, false, false, true));
  s.slots[1] = test::kOurs;
  EXPECT_TRUE(AnythingToUninstall(s, false, false, false, false));
  s.slots[1] = test::kOther;  // another handler's slot is not ours to remove
  EXPECT_FALSE(AnythingToUninstall(s, false, false, false, false));
  s.task.exists = true;
  EXPECT_TRUE(AnythingToUninstall(s, false, false, false, false));
  EXPECT_TRUE(AnythingToUninstall(test::MachineInstallSnapshot(), false, false, false, false));
  EXPECT_TRUE(AnythingToUninstall(test::UserInstallSnapshot(), false, false, false, false));
}

// ---- The commands' texts ----------------------------------------------------------------------

TEST(InstallTexts, Refusals) {
  InstallResult result;
  result.source.dir = L"D:\\Home\\someone\\build";
  for (InstallRefusal refusal :
       {InstallRefusal::kElevated, InstallRefusal::kInnoInstall, InstallRefusal::kNotABuild,
        InstallRefusal::kInsideRoot, InstallRefusal::kUnsafeFolder, InstallRefusal::kV1Machine}) {
    result.refusal = refusal;
    const std::wstring text = InstallRefusalMessage(result);
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.find(L"later"), std::wstring::npos) << text;
  }
  result.refusal = InstallRefusal::kElevated;
  EXPECT_EQ(InstallRefusalMessage(result).rfind(L"Cannot run here: ", 0), 0u);

  result.refusal = InstallRefusal::kNotABuild;
  result.sourceCheck.problem = SourceProblem::kMissingFile;
  result.sourceCheck.file = kPdfiumDll;
  EXPECT_EQ(InstallRefusalMessage(result), L"Cannot install from D:\\Home\\someone\\build: " +
                                               std::wstring(kPdfiumDll) +
                                               L" is not there. Run --install from the folder of "
                                               L"a build.");
  result.sourceCheck.problem = SourceProblem::kWrongArch;
  result.sourceCheck.arch = Arch::kArm64;
  EXPECT_NE(InstallRefusalMessage(result).find(L"built for arm64"), std::wstring::npos);

  result.refusal = InstallRefusal::kUnsafeFolder;
  result.unsafe = UnsafeAccess{L"D:\\Home", BroadAccess{L"Authenticated Users", DELETE}};
  const std::wstring unsafe = InstallRefusalMessage(result);
  EXPECT_NE(unsafe.find(L"D:\\Home can be changed by Authenticated Users"), std::wstring::npos)
      << unsafe;
  EXPECT_NE(unsafe.find(L"for every user"), std::wstring::npos) << unsafe;
  result.scope = Scope::kUser;
  const std::wstring unsafeUser = InstallRefusalMessage(result);
  EXPECT_NE(unsafeUser.find(L"your sign-in task"), std::wstring::npos) << unsafeUser;
  EXPECT_EQ(unsafeUser.find(L"for every user"), std::wstring::npos) << unsafeUser;
  result.scope = Scope::kNone;
  result.unsafe.reset();
  EXPECT_NE(InstallRefusalMessage(result).find(L"the step for all users found"),
            std::wstring::npos);

  result.refusal = InstallRefusal::kInnoInstall;
  result.inno = InnoInstall{Scope::kUser, L""};
  EXPECT_NE(InstallRefusalMessage(result).find(L"for this user"), std::wstring::npos);

  result.refusal = InstallRefusal::kV1Machine;
  const std::wstring v1 = InstallRefusalMessage(result);
  EXPECT_NE(v1.find(L"--install --machine"), std::wstring::npos);
  EXPECT_NE(v1.find(L"--force"), std::wstring::npos);
}

TEST(UninstallTexts, Report) {
  EXPECT_EQ(UninstallElevatedRefusalMessage().rfind(L"Cannot run here: ", 0), 0u);
  UninstallResult result;
  std::vector<std::wstring> lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0],
            std::wstring(kProductName) + L" removed. PDF files show their app icon again.");

  result.rebuildNotAvailable = true;
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kCancelled;
  result.machineLeft = true;
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].rfind(L"--rebuild-thumbnails: this build cannot", 0), 0u) << lines[0];
  EXPECT_NE(lines[1].find(L"declined"), std::wstring::npos);
  EXPECT_NE(lines[2].find(L"still installed"), std::wstring::npos);

  result = UninstallResult{};
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kFailed;
  result.run->failure = E_ACCESSDENIED;
  result.run->report = {L"failed folder: 0x80070005"};
  result.failures = {L"machine: 0x80070005"};
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], L"The part for all users failed: 0x80070005");
  EXPECT_EQ(lines[1], L"  failed folder: 0x80070005");

  result = UninstallResult{};
  result.machineNoProgram = true;
  result.machineLeft = true;
  result.files.scheduled = {L"D:\\Home\\someone\\x"};
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_NE(lines[0].find(L"no " + std::wstring(kSettingsExe)), std::wstring::npos);
  EXPECT_EQ(lines[0].find(L"--install") != std::wstring::npos, kDevInstall);
  EXPECT_EQ(lines[1], L"Some files are in use; they are removed when you next sign in.");
}

TEST(ZoomTexts, OriginAndSaved) {
  ZoomDecision decision;
  decision.zoom = 130;
  decision.origin = ZoomOrigin::kRegistry;
  EXPECT_EQ(ZoomText(decision), L"Zoom: 130 % (saved setting)");
  decision.origin = ZoomOrigin::kLegacyIni;
  EXPECT_EQ(ZoomText(decision), L"Zoom: 130 % (1.x's settings.ini)");
  decision.zoom = 100;
  decision.origin = ZoomOrigin::kDefault;
  EXPECT_EQ(ZoomText(decision), L"Zoom: 100 % (default)");
  const std::wstring saved = ZoomSavedText(150);
  EXPECT_EQ(saved.rfind(L"Zoom set to 150 %.", 0), 0u) << saved;
  // The thumbnail refresh is not in this build: nothing may send the user to it.
  EXPECT_EQ(saved.find(L"--refresh"), std::wstring::npos);
  EXPECT_EQ(saved.find(L"--"), std::wstring::npos);
}

TEST(UninstallTexts, NeedsYesWithoutAConsole) {
  CliArgs args;
  args.verb = Verb::kUninstall;
  EXPECT_TRUE(UninstallNeedsYes(args, false));
  EXPECT_FALSE(UninstallNeedsYes(args, true));
  args.yes = true;
  EXPECT_FALSE(UninstallNeedsYes(args, false));
  args.verb = Verb::kInstall;
  args.yes = false;
  EXPECT_FALSE(UninstallNeedsYes(args, false));
}

}  // namespace
}  // namespace pdft
