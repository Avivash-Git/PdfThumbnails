// The install and uninstall side's pure parts: the files of a build and what of them is held for
// the copy, the versions it installs, how the administrator step's outcome becomes --install's exit
// code (a step without administrator rights, or refused below an app package, included), the class
// path the elevated side may write for all users, the user hives it cleans, the per-user files the
// uninstall removes, the trash folders the status reports, and the texts of --install, --uninstall,
// --uninstall-cleanup, --zoom and --refresh.

#include <windows.h>

#include "install.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "commands.h"
#include "context.h"
#include "file_ops.h"
#include "project_config.h"
#include "rebuild.h"
#include "refresh.h"
#include "registration.h"
#include "snapshot_fixtures.h"
#include "strings.h"
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

TEST(Install, HoldsWhatIsCopied) {
  const BuildFolderHoldOptions hold = BuildHoldOptions();
  ASSERT_EQ(hold.files.size(), BuildBinaries().size());
  for (size_t i = 0; i < hold.files.size(); ++i) EXPECT_EQ(hold.files[i], BuildBinaries()[i]);
  ASSERT_EQ(hold.optional.size(), BuildTopNotices().size());
  for (size_t i = 0; i < hold.optional.size(); ++i) {
    EXPECT_EQ(hold.optional[i], BuildTopNotices()[i]);
  }
  EXPECT_EQ(hold.tree, std::wstring_view(kLicensesFolder));
  EXPECT_EQ(hold.treeDepth, kMaxNoticeDepth);
  EXPECT_TRUE(hold.user.empty());  // this process's user
  EXPECT_TRUE(hold.checkAccess);
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

// A run that was launched (|launched| S_OK) or not, and ended with |exitCode|, read the way
// RunElevated reads it.
ElevatedRun StepRun(DWORD exitCode, HRESULT launched = S_OK) {
  ElevatedRun run;
  run.launched = launched;
  run.exitCode = exitCode;
  run.outcome = ClassifyElevatedExit(launched, exitCode, &run.failure);
  return run;
}

TEST(Install, StepExitCodes) {
  EXPECT_FALSE(InstallStepExitCode(StepRun(0)));
  // A restart pending is no exit code of its own: the status reports it.
  EXPECT_FALSE(InstallStepExitCode(StepRun(1)));
  EXPECT_EQ(InstallStepExitCode(StepRun(0, HRESULT_FROM_WIN32(ERROR_CANCELLED))),
            ExitCode::kCancelled);
  EXPECT_EQ(InstallStepExitCode(StepRun(static_cast<DWORD>(kUnsafeBuildFolderHr))),
            ExitCode::kWrongContext);
  EXPECT_EQ(InstallStepExitCode(StepRun(static_cast<DWORD>(E_ACCESSDENIED))), ExitCode::kFailed);
  EXPECT_EQ(InstallStepExitCode(StepRun(static_cast<DWORD>(E_UNEXPECTED))), ExitCode::kFailed);
  // Started without administrator rights: neither a declined prompt nor an unsafe folder.
  EXPECT_EQ(InstallStepExitCode(StepRun(static_cast<DWORD>(kNoAdministratorRightsHr))),
            ExitCode::kNeedsAdmin);
  // Refused below an app package: cannot run here, like an unsafe folder.
  EXPECT_EQ(InstallStepExitCode(StepRun(static_cast<DWORD>(kBelowPackageHr))),
            ExitCode::kWrongContext);
  // The step's own codes count only from a step that was launched: a launch that failed with one
  // of them ran nothing and failed.
  EXPECT_EQ(InstallStepExitCode(StepRun(0, kUnsafeBuildFolderHr)), ExitCode::kFailed);
  EXPECT_EQ(InstallStepExitCode(StepRun(0, kBelowPackageHr)), ExitCode::kFailed);
}

// What a command says for an administrator step that is done: with a restart pending, and with
// parts it did not complete (without the restart's clause then: a part may be a file in use
// that could not be queued for deletion at the restart).
TEST(ElevatedTexts, DoneTexts) {
  ElevatedRun run;
  run.outcome = ElevatedOutcome::kDone;
  run.report = {ElevatedEndLine(ElevatedVerb::kMachineInstall, S_OK, false, 0)};
  run.parts = ReadReportParts(ElevatedVerb::kMachineInstall, run.report);
  EXPECT_EQ(ElevatedDoneText(L"The part for all users", run), L"The part for all users is done.");
  run.outcome = ElevatedOutcome::kRestartPending;
  EXPECT_EQ(ElevatedDoneText(L"The part for all users", run),
            L"The part for all users is done; files that were in use are deleted at the next "
            L"restart.");
  run.report = {NotDoneLine(L"restart-delete: 0x80070005"),
                ElevatedEndLine(ElevatedVerb::kMachineInstall, S_OK, true, 1)};
  run.parts = ReadReportParts(ElevatedVerb::kMachineInstall, run.report);
  ASSERT_TRUE(PartsNotDone(run));
  EXPECT_EQ(ElevatedDoneText(L"The part for all users", run),
            L"The part for all users is done, but some of its parts did not complete; its "
            L"report:");
  run.outcome = ElevatedOutcome::kDone;
  EXPECT_EQ(ElevatedDoneText(L"The administrator step", run),
            L"The administrator step is done, but some of its parts did not complete; its "
            L"report:");
}

TEST(Install, NoAdministratorRightsMessage) {
  const std::wstring text = NoAdministratorRightsMessage();
  EXPECT_EQ(text.rfind(L"The step for all users did not get administrator rights", 0), 0u);
  EXPECT_EQ(text.find(L"declined"), std::wstring::npos);
}

// Says that nothing changed and where to run the command instead; true of a step of an earlier
// build (refused below any package) and of this one (its test writes did not come back).
TEST(Install, BelowPackageRefusalMessage) {
  const std::wstring text = BelowPackageRefusalMessage();
  EXPECT_NE(text.find(L"changed nothing"), std::wstring::npos) << WideToUtf8Lossy(text);
  EXPECT_NE(text.find(L"Win+R"), std::wstring::npos) << WideToUtf8Lossy(text);
  EXPECT_NE(text.find(L"app package"), std::wstring::npos) << WideToUtf8Lossy(text);
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

  // A rebuild that was refused: one line first, before the part for all users.
  result.rebuild = RebuildResult{};
  result.rebuild->refusal = RebuildRefusal::kFileOperation;
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kCancelled;
  result.machineLeft = true;
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0],
            L"The thumbnail cache was not rebuilt: File Explorer is copying, moving or deleting "
            L"files.");
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

  // Started without administrator rights: said so, with the step's own refusal line after it.
  result = UninstallResult{};
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kFailed;
  result.run->failure = kNoAdministratorRightsHr;
  result.run->report = {
      L"elevated machine-uninstall refused hr=0x800402E4: no administrator rights"};
  result.machineLeft = true;
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].rfind(NoAdministratorRightsMessage(), 0), 0u) << WideToUtf8Lossy(lines[0]);
  EXPECT_NE(lines[0].find(L"stays"), std::wstring::npos);
  EXPECT_EQ(lines[1], L"  " + result.run->report[0]);
  EXPECT_NE(lines[2].find(L"still installed"), std::wstring::npos);

  // Refused below an app package: said so, with where to run it again, then the step's own
  // refusal line.
  result = UninstallResult{};
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kFailed;
  result.run->failure = kBelowPackageHr;
  result.run->report = {
      L"elevated machine-uninstall refused hr=0x8007109C: inside or below an app package"};
  result.machineLeft = true;
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].rfind(BelowPackageRefusalMessage(), 0), 0u) << WideToUtf8Lossy(lines[0]);
  EXPECT_NE(lines[0].find(L"stays"), std::wstring::npos);
  EXPECT_NE(lines[0].find(L"--uninstall again"), std::wstring::npos);
  EXPECT_EQ(lines[1], L"  " + result.run->report[0]);
  EXPECT_NE(lines[2].find(L"still installed"), std::wstring::npos);

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

// The question names the restart of File Explorer only when the uninstall rebuilds the cache.
TEST(UninstallTexts, Question) {
  EXPECT_EQ(UninstallQuestion(false),
            L"Remove " + std::wstring(kProductName) + L" for all users of this PC?");
  const std::wstring rebuild = UninstallQuestion(true);
  EXPECT_EQ(rebuild, L"Remove " + std::wstring(kProductName) +
                         L" for all users of this PC, and rebuild the thumbnail cache (restarts "
                         L"File Explorer)?");
  EXPECT_EQ(rebuild.find(L"[y/N]"), std::wstring::npos);
}

// A rebuild that ran prints the rebuild's own report before the uninstall's lines; one that was
// refused or could not start is one line saying why, never "Nothing changed." (the uninstall went
// on) and never a command to run (this program is gone afterwards).
TEST(UninstallTexts, TheThumbnailRebuild) {
  UninstallResult result;
  result.rebuild = RebuildResult{};
  result.rebuild->committed = true;
  result.rebuild->cacheFiles = 3;
  result.rebuild->deleted = 3;
  result.rebuild->ended = 1;
  result.rebuild->shell = ShellOutcome::kBack;
  const std::vector<std::wstring> rebuilt = RebuildReport(*result.rebuild);
  std::vector<std::wstring> lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), rebuilt.size() + 1);
  EXPECT_TRUE(std::equal(rebuilt.begin(), rebuilt.end(), lines.begin()));
  EXPECT_EQ(lines[0], L"Thumbnail cache cleared: 3 of 3 cache files deleted.");
  EXPECT_EQ(lines.back(),
            std::wstring(kProductName) + L" removed. PDF files show their app icon again.");

  // Not back: the rebuild's line says what to do; the failure it is listed as keeps the last line.
  result.rebuild->shell = ShellOutcome::kNotBack;
  result.rebuild->start = ShellStart::kBelowPackage;
  result.failures = {L"rebuild: 0x800705B4"};
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), RebuildReport(*result.rebuild).size());
  EXPECT_TRUE(std::any_of(lines.begin(), lines.end(), [](const std::wstring& line) {
    return line.starts_with(L"File Explorer did not start again. Start it from Task Manager");
  }));

  const auto notRebuilt = [](const RebuildResult& rebuild) {
    UninstallResult uninstall;
    uninstall.rebuild = rebuild;
    const std::vector<std::wstring> report = UninstallReport(uninstall);
    EXPECT_EQ(report.size(), 2u);
    return report.empty() ? std::wstring() : report[0];
  };
  for (const RebuildRefusal refusal :
       {RebuildRefusal::kElevated, RebuildRefusal::kVirtualized, RebuildRefusal::kOtherShellUser,
        RebuildRefusal::kNoRestartBelowPackage, RebuildRefusal::kNoRestartInJob,
        RebuildRefusal::kNoRestartStartFails, RebuildRefusal::kOwnProcessHoldsCache,
        RebuildRefusal::kFileOperation, RebuildRefusal::kCancelled}) {
    RebuildResult rebuild;
    rebuild.refusal = refusal;
    const std::wstring line = notRebuilt(rebuild);
    EXPECT_EQ(line.rfind(L"The thumbnail cache was not rebuilt: ", 0), 0u)
        << static_cast<int>(refusal);
    EXPECT_TRUE(line.ends_with(L".")) << static_cast<int>(refusal);
    EXPECT_EQ(line.find(L"Nothing changed"), std::wstring::npos) << WideToUtf8Lossy(line);
    EXPECT_EQ(line.find(L"--refresh"), std::wstring::npos) << WideToUtf8Lossy(line);
  }
  RebuildResult refused;
  refused.refusal = RebuildRefusal::kOtherShellUser;
  EXPECT_EQ(notRebuilt(refused),
            L"The thumbnail cache was not rebuilt: File Explorer on this desktop belongs to "
            L"another account, and the thumbnail cache is each account's own.");
  refused.refusal = RebuildRefusal::kNoRestartInJob;
  EXPECT_EQ(notRebuilt(refused),
            L"The thumbnail cache was not rebuilt: Windows is set not to restart File Explorer by "
            L"itself, and a File Explorer started from here would stay inside a job of the program "
            L"this runs in.");
  refused.refusal = RebuildRefusal::kNoRestartStartFails;
  refused.startError = E_FAIL;
  EXPECT_EQ(notRebuilt(refused),
            L"The thumbnail cache was not rebuilt: Windows is set not to restart File Explorer by "
            L"itself, and this program cannot start File Explorer from here either (0x80004005).");

  RebuildResult failed;
  failed.failure = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  EXPECT_NE(notRebuilt(failed).find(L"link or junction"), std::wstring::npos);
  failed.failure = E_FAIL;
  EXPECT_EQ(notRebuilt(failed), L"The thumbnail cache was not rebuilt: 0x80004005.");
}

// Keys left in other users' hives, with the class and the install folders gone, are said as
// such; with the part for all users still there, it is an ordinary failure.
TEST(UninstallTexts, KeysLeftInOtherUsersHives) {
  UninstallResult result;
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kFailed;
  result.run->failure = kRegistryTreeLeftHr;
  result.run->report = {L"failed hive left: " + HResultText(kRegistryTreeLeftHr)};
  result.failures = {L"machine left: " + HResultText(kRegistryTreeLeftHr)};
  std::vector<std::wstring> lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].rfind(L"Removed the part for all users, but some keys", 0), 0u)
      << WideToUtf8Lossy(lines[0]);
  EXPECT_EQ(lines[1], L"  " + result.run->report[0]);

  result.machineLeft = true;
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], L"The part for all users failed: " + HResultText(kRegistryTreeLeftHr));
  EXPECT_NE(lines[2].find(L"still installed"), std::wstring::npos);
}

// Another process had taken the report pipe's name: said after the outcome, whatever the step's
// own exit code says, but not for a declined prompt (no step ran).
TEST(UninstallTexts, AReportChannelTakenByAnotherProcess) {
  UninstallResult result;
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kDone;
  result.run->reportServer = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  std::vector<std::wstring> lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], L"Removed the part for all users.");
  EXPECT_EQ(lines[1], ReportChannelTakenMessage());

  result.run->outcome = ElevatedOutcome::kFailed;
  result.run->failure = E_FAIL;
  result.failures = {L"machine: " + HResultText(E_FAIL)};
  lines = UninstallReport(result);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], L"The part for all users failed: " + HResultText(E_FAIL));
  EXPECT_EQ(lines[1], ReportChannelTakenMessage());

  result = UninstallResult{};
  result.run = ElevatedRun{};
  result.run->outcome = ElevatedOutcome::kCancelled;
  result.run->reportServer = HRESULT_FROM_WIN32(ERROR_PIPE_BUSY);
  result.machineLeft = true;
  for (const std::wstring& line : UninstallReport(result)) {
    EXPECT_NE(line, ReportChannelTakenMessage());
  }

  // A channel nobody took says nothing.
  result.run->outcome = ElevatedOutcome::kDone;
  result.run->reportServer = S_OK;
  result.machineLeft = false;
  for (const std::wstring& line : UninstallReport(result)) {
    EXPECT_NE(line, ReportChannelTakenMessage());
  }
}

// --uninstall-cleanup says nothing when it did everything; otherwise its error output says what
// it left out and why, each failure, and that it stopped early.
TEST(UninstallTexts, CleanupSaysWhatItLeftOut) {
  UninstallCleanupResult result;
  result.userPart = true;
  result.machinePart = true;
  EXPECT_TRUE(UninstallCleanupErrors(S_OK, result).empty());

  // Below an app package where its test writes did not come back: the part for all users is left
  // out, and so is this account's, whose registry check found its writes virtualized.
  result = UninstallCleanupResult{};
  result.machineRefused = true;
  result.machineWrites.packageAbove = true;
  result.machineWrites.probed = true;
  result.machineWrites.machine = Virtualization::kYes;
  result.machineWrites.user = Virtualization::kYes;
  result.userWrites = Virtualization::kYes;
  result.failures = {L"machine: " + HResultText(kBelowPackageHr)};
  std::vector<std::wstring> lines = UninstallCleanupErrors(S_OK, result);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0],
            L"The part for all users was left out: started below an app package (or unable to "
            L"read the processes above it), its test writes did not come back through WMI (app "
            L"package above; HKLM virtualized, HKCU virtualized).");
  EXPECT_EQ(lines[1],
            L"The part for this account was left out: the registry check (a test value written "
            L"here and read back through WMI) did not show that changes made from here would "
            L"reach Windows (virtualized=yes).");
  EXPECT_EQ(lines[2], L"Not done: machine: 0x8007109C");

  // Elevated without full administrator rights: the part for all users is left out, and says why.
  result = UninstallCleanupResult{};
  result.userPart = true;
  result.machineNoRights = true;
  result.failures = {L"machine: " + HResultText(kNoAdministratorRightsHr)};
  lines = UninstallCleanupErrors(S_OK, result);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0],
            L"The part for all users was left out: this process is elevated, but without full "
            L"administrator rights (the Administrators group enabled and an integrity level of "
            L"High or more).");
  EXPECT_EQ(lines[1], L"Not done: machine: 0x800402E4");

  // A registry check that failed as a whole names its failure.
  result = UninstallCleanupResult{};
  result.userProbeError = static_cast<HRESULT>(0x80041003);
  lines = UninstallCleanupErrors(S_OK, result);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find(L"(virtualized=unknown, probe-hr=0x80041003)."), std::wstring::npos)
      << WideToUtf8Lossy(lines[0]);

  // Stopped early: said with its HRESULT, before the failures; a per-user part it may never have
  // reached is not put down to the registry check.
  result = UninstallCleanupResult{};
  result.machinePart = true;
  result.failures = {L"machine class: 0x80070005"};
  lines = UninstallCleanupErrors(E_OUTOFMEMORY, result);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], L"The clean-up stopped early: 0x8007000E");
  EXPECT_EQ(lines[1], L"Not done: machine class: 0x80070005");
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
  // The full rebuild, with what it costs, and the refresh of one folder's PDF thumbnails.
  EXPECT_NE(saved.find(L"run --refresh --full (restarts File Explorer), or --refresh --folder "
                       L"<folder> for the PDF thumbnails in one folder."),
            std::wstring::npos)
      << WideToUtf8Lossy(saved);
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

// Only the full rebuild asks; --refresh without --full has no question to answer.
TEST(RefreshTexts, NeedsYesWithoutAConsole) {
  CliArgs args;
  args.verb = Verb::kRefresh;
  args.full = true;
  EXPECT_TRUE(RefreshNeedsYes(args, false));
  EXPECT_FALSE(RefreshNeedsYes(args, true));
  args.yes = true;
  EXPECT_FALSE(RefreshNeedsYes(args, false));
  args.yes = false;
  args.full = false;
  EXPECT_FALSE(RefreshNeedsYes(args, false));
  args.folders = {L"C:\\a"};
  EXPECT_FALSE(RefreshNeedsYes(args, false));
  args.verb = Verb::kUninstall;
  args.folders.clear();
  args.full = true;
  EXPECT_FALSE(RefreshNeedsYes(args, false));
}

// A registry that may be virtualized gets the line every writing command gives there; the
// rebuild's own refusals get its texts; a stop before anything changed has no refusal line.
TEST(RefreshTexts, Refusals) {
  ProcessContext context;
  context.userAccount = L"PC\\tester";
  context.packagedAncestor = L"Example_1.0.0.0_x64__abcdefghijklm";
  context.virtualization = Virtualization::kYes;
  EXPECT_EQ(RefreshFullRefusalMessage(RebuildRefusal::kVirtualized, context),
            CannotRunHereMessage(context));
  EXPECT_NE(RefreshFullRefusalMessage(RebuildRefusal::kVirtualized, context)
                .find(L"Example_1.0.0.0_x64__abcdefghijklm"),
            std::wstring::npos);
  for (const RebuildRefusal refusal :
       {RebuildRefusal::kElevated, RebuildRefusal::kOtherShellUser,
        RebuildRefusal::kNoRestartBelowPackage, RebuildRefusal::kNoRestartInJob,
        RebuildRefusal::kNoRestartStartFails, RebuildRefusal::kOwnProcessHoldsCache,
        RebuildRefusal::kFileOperation}) {
    const std::wstring message = RefreshFullRefusalMessage(refusal, context);
    EXPECT_FALSE(message.empty()) << static_cast<int>(refusal);
    EXPECT_EQ(message, RebuildRefusalText(refusal, context));
  }
  // Where File Explorer could not be started again: nothing changed, and the start's own error.
  const std::wstring inJob = RefreshFullRefusalMessage(RebuildRefusal::kNoRestartInJob, context);
  EXPECT_NE(inJob.find(L"Nothing changed"), std::wstring::npos) << WideToUtf8Lossy(inJob);
  EXPECT_NE(inJob.find(L"Win+R"), std::wstring::npos) << WideToUtf8Lossy(inJob);
  const std::wstring startFails =
      RefreshFullRefusalMessage(RebuildRefusal::kNoRestartStartFails, context, E_ACCESSDENIED);
  EXPECT_NE(startFails.find(L"(" + HResultText(E_ACCESSDENIED) + L")"), std::wstring::npos)
      << WideToUtf8Lossy(startFails);
  EXPECT_TRUE(startFails.ends_with(L"Nothing changed.")) << WideToUtf8Lossy(startFails);
  EXPECT_EQ(RefreshFullRefusalMessage(RebuildRefusal::kElevated, context)
                .rfind(L"Cannot run here: running as administrator.", 0),
            0u);
  EXPECT_NE(RefreshFullRefusalMessage(RebuildRefusal::kOtherShellUser, context).find(L"PC\\tester"),
            std::wstring::npos);
  EXPECT_TRUE(RefreshFullRefusalMessage(RebuildRefusal::kCancelled, context).empty());
  EXPECT_TRUE(RefreshFullRefusalMessage(RebuildRefusal::kNone, context).empty());
}

// --refresh --folder: the same line where the registry may be virtualized, its own texts for the
// rest.
TEST(RefreshTexts, FolderRefusals) {
  ProcessContext context;
  context.userAccount = L"PC\\tester";
  context.packagedAncestor = L"Example_1.0.0.0_x64__abcdefghijklm";
  context.virtualization = Virtualization::kYes;
  EXPECT_EQ(RefreshFolderRefusalMessage(RefreshRefusal::kVirtualized, context),
            CannotRunHereMessage(context));
  for (const RefreshRefusal refusal :
       {RefreshRefusal::kElevated, RefreshRefusal::kOtherShellUser, RefreshRefusal::kNotInstalled,
        RefreshRefusal::kNotOurHandler, RefreshRefusal::kThumbnailsOff}) {
    const std::wstring message = RefreshFolderRefusalMessage(refusal, context);
    EXPECT_FALSE(message.empty()) << static_cast<int>(refusal);
    EXPECT_EQ(message, RefreshRefusalText(refusal, context));
  }
  EXPECT_EQ(RefreshFolderRefusalMessage(RefreshRefusal::kElevated, context)
                .rfind(L"Cannot run here: running as administrator.", 0),
            0u);
  EXPECT_NE(RefreshFolderRefusalMessage(RefreshRefusal::kOtherShellUser, context).find(L"PC\\tester"),
            std::wstring::npos);
  EXPECT_TRUE(RefreshFolderRefusalMessage(RefreshRefusal::kNone, context).empty());
}

}  // namespace
}  // namespace pdft
