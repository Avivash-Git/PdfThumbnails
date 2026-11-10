// The pure decisions behind the process context and the commands (context.h, commands.h,
// instance.h, window_messages.h): registry virtualization verdicts (the elevated side's below an
// app package too), the refusal message, named-object descriptors and names, what a refused
// creation of a named mutex means, whose settings window mutex and window count, and when the
// window may start again through File Explorer.

#include <windows.h>

#include <gtest/gtest.h>
#include <sddl.h>

#include <optional>
#include <string>
#include <vector>

#include "commands.h"
#include "context.h"
#include "instance.h"
#include "project_config.h"
#include "window_messages.h"

namespace pdft {
namespace {

using enum Virtualization;

TEST(Virtualization, Canary) {
  EXPECT_EQ(DecideCanary(false, false, std::nullopt, L"v"), kUnknown);  // could not write
  EXPECT_EQ(DecideCanary(false, true, std::wstring(L"v"), L"v"), kUnknown);
  EXPECT_EQ(DecideCanary(true, false, std::nullopt, L"v"), kUnknown);  // WMI failed
  EXPECT_EQ(DecideCanary(true, true, std::nullopt, L"v"), kYes);       // not in the real hive
  EXPECT_EQ(DecideCanary(true, true, std::wstring(L"other"), L"v"), kYes);
  EXPECT_EQ(DecideCanary(true, true, std::wstring(L""), L"v"), kYes);
  EXPECT_EQ(DecideCanary(true, true, std::wstring(L"v"), L"v"), kNo);
}

TEST(Virtualization, UnknownCountsAsYesForWrites) {
  EXPECT_FALSE(RefusesWrites(kNo));
  EXPECT_TRUE(RefusesWrites(kYes));
  EXPECT_TRUE(RefusesWrites(kUnknown));
}

MachineWriteCheck Above(bool packageAbove, HRESULT walk) {
  MachineWriteCheck check;
  check.packageAbove = packageAbove;
  check.walk = walk;
  return check;
}

MachineWriteCheck Probed(MachineWriteCheck check, Virtualization machine, Virtualization user,
                         HRESULT probeError = S_OK) {
  check.probed = true;
  check.machine = machine;
  check.user = user;
  check.probeError = probeError;
  return check;
}

// A step for all users refuses below a package (or where the walk failed) unless both of its test
// writes came back real; without one, whatever the canaries would say.
TEST(Virtualization, MachineWrites) {
  struct Row {
    const char* name;
    MachineWriteCheck check;
    bool below;
    bool refuses;
  };
  const Row rows[] = {
      {"NoPackage", Above(false, S_OK), false, false},
      {"NoPackageVirtualized", Probed(Above(false, S_OK), kYes, kYes), false, false},
      {"PackageNotChecked", Above(true, S_OK), true, true},
      {"WalkFailedNotChecked", Above(false, E_ACCESSDENIED), true, true},
      {"PackageBothReal", Probed(Above(true, S_OK), kNo, kNo), true, false},
      {"WalkFailedBothReal", Probed(Above(false, E_ACCESSDENIED), kNo, kNo), true, false},
      {"PackageMachineVirtualized", Probed(Above(true, S_OK), kYes, kNo), true, true},
      {"PackageUserVirtualized", Probed(Above(true, S_OK), kNo, kYes), true, true},
      {"PackageMachineUnknown", Probed(Above(true, S_OK), kUnknown, kNo), true, true},
      {"PackageUserUnknown", Probed(Above(true, S_OK), kNo, kUnknown), true, true},
  };
  for (const Row& row : rows) {
    EXPECT_EQ(BelowPackage(row.check), row.below) << row.name;
    EXPECT_EQ(RefusesMachineWrites(row.check), row.refuses) << row.name;
  }
}

// What a step reports about its context: never a package name or a path.
TEST(Virtualization, MachineWriteCheckText) {
  EXPECT_EQ(MachineWriteCheckText(Above(false, S_OK)), L"no app package above");
  EXPECT_EQ(MachineWriteCheckText(Above(true, S_OK)), L"app package above; not checked");
  EXPECT_EQ(MachineWriteCheckText(Probed(Above(true, S_OK), kNo, kNo)),
            L"app package above; HKLM real, HKCU real");
  EXPECT_EQ(MachineWriteCheckText(
                Probed(Above(true, S_OK), kUnknown, kNo, static_cast<HRESULT>(0x80041003))),
            L"app package above; HKLM unknown, HKCU real, probe-hr=0x80041003");
  EXPECT_EQ(MachineWriteCheckText(Probed(Above(false, E_ACCESSDENIED), kNo, kYes)),
            L"processes above unreadable, walk-hr=0x80070005; HKLM real, HKCU virtualized");
}

ValueView Missing() { return {ValueState::kMissing, {}}; }
ValueView Present(const wchar_t* data) { return {ValueState::kPresent, data}; }
ValueView Unreadable() { return {ValueState::kUnreadable, {}, E_ACCESSDENIED}; }

// The read-only check answers whether reads reach the real hive, in its own type: agreeing
// views are no verdict about writes.
TEST(RegistryViews, ReadOnlyComparison) {
  constexpr RegistryViews kAgree = RegistryViews::kAgree;
  constexpr RegistryViews kDiffer = RegistryViews::kDiffer;
  constexpr RegistryViews kUndecided = RegistryViews::kUnknown;
  struct Row {
    const char* name;
    std::vector<ValueViews> values;
    RegistryViews withoutAncestor;
    RegistryViews withAncestor;
  };
  const Row rows[] = {
      {"NothingToCompare", {}, kAgree, kUndecided},
      {"BothMissingOnBothSides",
       {{Missing(), Missing()}, {Missing(), Missing()}},
       kAgree,
       kUndecided},
      {"PresentAndEqual",
       {{Present(L"{a}"), Present(L"{a}")}, {Missing(), Missing()}},
       kAgree,
       kAgree},
      {"BothPresentAndEqual",
       {{Present(L"{a}"), Present(L"{a}")}, {Present(L"130"), Present(L"130")}},
       kAgree,
       kAgree},
      {"OnlyInside", {{Present(L"{a}"), Missing()}, {Missing(), Missing()}}, kDiffer, kDiffer},
      {"OnlyOutside", {{Missing(), Missing()}, {Missing(), Present(L"130")}}, kDiffer, kDiffer},
      {"Different", {{Present(L"{a}"), Present(L"{b}")}, {Missing(), Missing()}}, kDiffer, kDiffer},
      {"CaseMatters", {{Present(L"{A}"), Present(L"{a}")}}, kDiffer, kDiffer},
      {"Unreadable",
       {{Unreadable(), Present(L"{a}")}, {Missing(), Missing()}},
       kUndecided,
       kUndecided},
      {"UnreadableOutside",
       {{Present(L"{a}"), Present(L"{a}")}, {Missing(), Unreadable()}},
       kUndecided,
       kUndecided},
      {"DifferenceBeatsUnreadable",
       {{Unreadable(), Unreadable()}, {Present(L"1"), Missing()}},
       kDiffer,
       kDiffer},
  };
  for (const Row& row : rows) {
    EXPECT_EQ(DecideFromViews(row.values, false), row.withoutAncestor) << row.name;
    EXPECT_EQ(DecideFromViews(row.values, true), row.withAncestor) << row.name;
  }
}

TEST(Context, ArchFromMachine) {
  EXPECT_EQ(ArchFromMachine(IMAGE_FILE_MACHINE_AMD64), Arch::kX64);
  EXPECT_EQ(ArchFromMachine(IMAGE_FILE_MACHINE_ARM64), Arch::kArm64);
  EXPECT_EQ(ArchFromMachine(IMAGE_FILE_MACHINE_I386), Arch::kOther);
  EXPECT_EQ(ArchFromMachine(IMAGE_FILE_MACHINE_UNKNOWN), Arch::kOther);
}

TEST(Commands, CannotRunHereNamesTheAncestor) {
  ProcessContext context;
  context.packagedAncestor = L"Example_1.0.0.0_x64__abcdefghijklm";
  context.virtualization = kYes;
  const std::wstring message = CannotRunHereMessage(context);
  EXPECT_EQ(message.rfind(L"Cannot run here: ", 0), 0u) << message;
  EXPECT_NE(message.find(L"started from the packaged app Example_1.0.0.0_x64__abcdefghijklm; "
                         L"registry virtualized"),
            std::wstring::npos)
      << message;

  context.virtualization = kUnknown;
  const std::wstring packagedUnknown = CannotRunHereMessage(context);
  EXPECT_NE(packagedUnknown.find(L"registry virtualization could not be ruled out"),
            std::wstring::npos)
      << packagedUnknown;
  EXPECT_NE(packagedUnknown.find(L"terminal that is not part of an app package"),
            std::wstring::npos)
      << packagedUnknown;
}

// On a normal PC an undecided canary means that the check itself failed: the message says so,
// with the HRESULT, and gives no advice about leaving a container that is not there.
TEST(Commands, CannotRunHereWhenTheCheckFailedOnANormalPc) {
  ProcessContext context;
  context.virtualization = kUnknown;
  context.probeError = HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
  const std::wstring message = CannotRunHereMessage(context);
  EXPECT_EQ(message.rfind(L"Cannot run here: ", 0), 0u) << message;
  EXPECT_NE(message.find(L"registry check"), std::wstring::npos) << message;
  EXPECT_NE(message.find(L"WMI"), std::wstring::npos) << message;
  EXPECT_NE(message.find(L"0x800706BA"), std::wstring::npos) << message;
  EXPECT_NE(message.find(L"cannot be confirmed"), std::wstring::npos) << message;
  EXPECT_EQ(message.find(L"packaged app"), std::wstring::npos) << message;
  EXPECT_EQ(message.find(L"terminal"), std::wstring::npos) << message;

  context.probeError = S_OK;  // no failure recorded: still no advice, and no HRESULT
  const std::wstring bare = CannotRunHereMessage(context);
  EXPECT_EQ(bare.find(L"0x"), std::wstring::npos) << bare;
  EXPECT_EQ(bare.find(L"terminal"), std::wstring::npos) << bare;

  // A canary that disagreed on a normal PC is still "virtualized".
  context.virtualization = kYes;
  EXPECT_NE(CannotRunHereMessage(context).find(L"registry virtualized"), std::wstring::npos);
}

// --check and --fix refuse an elevated process with a message of their own, worded like the
// others (scripts look for the prefix) and pointing to a normal prompt.
TEST(Commands, CannotRunHereWhenElevated) {
  const std::wstring message = ElevatedRefusalMessage();
  EXPECT_EQ(message.rfind(L"Cannot run here: running as administrator", 0), 0u) << message;
  EXPECT_NE(message.find(L"normal (not administrator) prompt"), std::wstring::npos) << message;
}

// The settings window never opens elevated, and says how to open it instead.
TEST(Commands, WindowCannotOpenElevated) {
  const std::wstring message = WindowElevatedRefusalMessage();
  EXPECT_EQ(message.rfind(L"Cannot run here: running as administrator", 0), 0u) << message;
  EXPECT_NE(message.find(L"signed-in user's own settings"), std::wstring::npos) << message;
  EXPECT_NE(message.find(L"Open it normally (not as administrator)"), std::wstring::npos)
      << message;
}

// When the window cannot start again through File Explorer, it says how to open it.
TEST(Commands, WindowRelaunchFailureSaysHowToOpenIt) {
  const std::wstring message = WindowRelaunchFailedMessage();
  EXPECT_NE(message.find(L"File Explorer or a normal terminal"), std::wstring::npos) << message;
}

// File Explorer starts the window again with its own token. For this process's own user any copy
// may start (leaving an app package); for another account only the install for all users' own
// executable, which that account cannot change.
TEST(Commands, OnlyTheInstallForAllUsersStartsAgainForAnotherAccount) {
  EXPECT_TRUE(MayRelaunchThroughShell(false, false));
  EXPECT_TRUE(MayRelaunchThroughShell(false, true));
  EXPECT_TRUE(MayRelaunchThroughShell(true, true));
  EXPECT_FALSE(MayRelaunchThroughShell(true, false));
}

// --checks refuses an elevated process too, saying why for the task.
TEST(Commands, ChecksCannotRunElevated) {
  const std::wstring message = ChecksElevatedRefusalMessage();
  EXPECT_EQ(message.rfind(L"Cannot run here: running as administrator", 0), 0u) << message;
  EXPECT_NE(message.find(L"check task"), std::wstring::npos) << message;
  EXPECT_NE(message.find(L"--checks from a normal (not administrator) prompt"), std::wstring::npos)
      << message;
}

TEST(Commands, ElevatedProcessesNeverLog) {
  ProcessContext context;
  context.elevated = false;
  EXPECT_TRUE(ShouldLog(context));
  context.elevated = true;
  EXPECT_FALSE(ShouldLog(context));
  context.packagedAncestor = L"Example_1.0.0.0_x64__abcdefghijklm";
  EXPECT_FALSE(ShouldLog(context));
}

// An elevated --status, --check or --fix leaves %TEMP% alone: the user's programs choose where
// it is.
TEST(Commands, ElevatedProcessesNeverSweepProbes) {
  ProcessContext context;
  context.elevated = false;
  EXPECT_TRUE(SweepsProbes(context));
  context.elevated = true;
  EXPECT_FALSE(SweepsProbes(context));
  context.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;  // Administrators enabled at Medium
  EXPECT_FALSE(SweepsProbes(context));
}

// Each registry check logs its own result under its own name.
TEST(Commands, StartMessageNamesTheCheckThatRan) {
  ProcessContext context;
  context.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
  EXPECT_EQ(StartMessage(Mode::kCli, context), L"start cli il=0x2000 elevated=no packaged=no");

  context.probe = ContextProbe::kWrite;
  context.virtualization = kYes;
  context.packagedAncestor = L"Example_1.0.0.0_x64__abcdefghijklm";
  EXPECT_EQ(StartMessage(Mode::kGui, context),
            L"start gui il=0x2000 elevated=no packaged=yes virtualized=yes");

  context.probe = ContextProbe::kReadOnly;
  context.virtualization = kNo;
  context.views = RegistryViews::kAgree;
  EXPECT_EQ(StartMessage(Mode::kCli, context),
            L"start cli il=0x2000 elevated=no packaged=yes views=agree");
  context.views = RegistryViews::kDiffer;
  EXPECT_NE(StartMessage(Mode::kCli, context).find(L" views=differ"), std::wstring::npos);

  context.views = RegistryViews::kUnknown;
  context.probeError = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  EXPECT_EQ(StartMessage(Mode::kCli, context),
            L"start cli il=0x2000 elevated=no packaged=yes views=unknown probe-hr=0x80070005");

  context.probe = ContextProbe::kWrite;
  context.virtualization = kUnknown;
  context.elevated = true;
  context.integrityRid = SECURITY_MANDATORY_HIGH_RID;
  EXPECT_EQ(StartMessage(Mode::kCli, context),
            L"start cli il=0x3000 elevated=yes packaged=yes virtualized=unknown "
            L"probe-hr=0x80070005");
}

TEST(Commands, DoubleClickStartsTheSettingsWindow) {
  EXPECT_TRUE(ShouldStartSettingsWindow(1, true));    // a console of its own: double-clicked
  EXPECT_FALSE(ShouldStartSettingsWindow(2, true));   // shared with a shell
  EXPECT_FALSE(ShouldStartSettingsWindow(1, false));  // hidden: started by a program
  EXPECT_FALSE(ShouldStartSettingsWindow(0, false));  // no console at all
}

TEST(Commands, VersionText) {
  EXPECT_EQ(VersionText(),
            std::wstring(kProductName) + L" " + kProductVersion + L" (" + kArch + L")");
}

TEST(Commands, TakingSlotsBackNeedsYesWithoutAConsole) {
  CliArgs args;
  args.verb = Verb::kFix;
  args.takeBack = true;
  EXPECT_TRUE(TakeBackNeedsYes(args, false));
  EXPECT_FALSE(TakeBackNeedsYes(args, true)) << "a console can be asked";
  args.yes = true;
  EXPECT_FALSE(TakeBackNeedsYes(args, false));
  args.yes = false;
  args.takeBack = false;
  EXPECT_FALSE(TakeBackNeedsYes(args, false)) << "a plain --fix takes nothing back";
  args.verb = Verb::kCheck;
  args.takeBack = true;
  EXPECT_FALSE(TakeBackNeedsYes(args, false));
}

TEST(Commands, YesAnswers) {
  for (const wchar_t* yes : {L"y", L"Y", L"yes", L"YES", L" y\r\n", L"\tYes "}) {
    EXPECT_TRUE(IsYesAnswer(yes)) << yes;
  }
  for (const wchar_t* no : {L"", L"\r\n", L"n", L"no", L"N", L"yy", L"ye", L"yes please", L"j"}) {
    EXPECT_FALSE(IsYesAnswer(no)) << no;
  }
}

TEST(NamedObjects, Descriptor) {
  // A made-up user SID in the Microsoft Entra form (S-1-12-1-...): the repository's publish check
  // rejects anything shaped like a local or domain account's SID, even an invented one.
  constexpr wchar_t kSid[] = L"S-1-12-1-1-2-3-4";
  EXPECT_EQ(NamedObjectSddl(kSid, false),
            L"D:(A;;GA;;;S-1-12-1-1-2-3-4)(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NWNRNX;;;ME)");
  EXPECT_EQ(NamedObjectSddl(kSid, true),
            L"D:(A;;GA;;;S-1-12-1-1-2-3-4)(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;WD)"
            L"S:(ML;;NWNRNX;;;ME)");
  for (bool everyone : {false, true}) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    EXPECT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        NamedObjectSddl(kSid, everyone).c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        << GetLastError();
    LocalFree(descriptor);
  }
}

TEST(NamedObjects, Names) {
  const std::wstring prefix = kObjectPrefix;
  EXPECT_EQ(SettingsInstanceMutexName(L"S-1-2"), L"Local\\" + prefix + L".Settings.S-1-2");
  EXPECT_EQ(OperationLockName(L"S-1-2"), L"Global\\" + prefix + L".Operation.S-1-2");
  EXPECT_EQ(OperationSessionLockName(L"S-1-2"), L"Local\\" + prefix + L".Operation.S-1-2");
  EXPECT_EQ(LogMutexName(L"S-1-2"), L"Local\\" + prefix + L".Log.S-1-2");
  EXPECT_EQ(AppMutexName(false), prefix + L"AppMutex");
  EXPECT_EQ(AppMutexName(true), L"Global\\" + prefix + L"AppMutex");
}

// Whose global lock may be trusted: one this user made, unelevated (the user owns it) or elevated
// (Administrators own it), or one SYSTEM made. Made-up SIDs in the Microsoft Entra form, as above.
TEST(NamedObjects, LockOwnerMadeByThisUser) {
  constexpr wchar_t kUser[] = L"S-1-12-1-1-2-3-4";
  EXPECT_TRUE(MadeByThisUser(kUser, kUser));
  EXPECT_TRUE(MadeByThisUser(L"S-1-5-32-544", kUser));       // Administrators
  EXPECT_TRUE(MadeByThisUser(L"S-1-5-18", kUser));           // SYSTEM
  EXPECT_FALSE(MadeByThisUser(L"S-1-12-1-1-2-3-5", kUser));  // another user
  EXPECT_FALSE(MadeByThisUser(L"S-1-12-1-1-2-3-44", kUser));
  EXPECT_FALSE(MadeByThisUser(L"S-1-12-1-1-2-3", kUser));  // a prefix of the user's SID
  EXPECT_FALSE(MadeByThisUser(L"S-1-5-32-545", kUser));    // Users
  EXPECT_FALSE(MadeByThisUser(L"S-1-5-19", kUser));        // LOCAL SERVICE
  EXPECT_FALSE(MadeByThisUser(L"S-1-1-0", kUser));         // Everyone
  EXPECT_FALSE(MadeByThisUser(L"", kUser));                // no owner could be read
}

// The settings window's mutex, found made already: another window of this user only when this
// user made it at Medium or above. Made-up SIDs in the Microsoft Entra form, as above.
TEST(NamedObjects, AnExistingInstanceMutexIsAWindowOnlyWhenThisUserMadeIt) {
  using enum SingleInstance;
  constexpr wchar_t kUser[] = L"S-1-12-1-1-2-3-4";
  EXPECT_EQ(ClassifyExistingInstanceMutex(kUser, kUser, false), kOtherWindow);
  EXPECT_EQ(ClassifyExistingInstanceMutex(L"S-1-5-32-544", kUser, false), kOtherWindow);
  // Made by a process of this user below Medium (a sandboxed one): not a window of theirs.
  EXPECT_EQ(ClassifyExistingInstanceMutex(kUser, kUser, true), kLowerIntegrity);
  // Another account's, or one whose owner could not be read: not a window of this user's either,
  // whatever its label says.
  EXPECT_EQ(ClassifyExistingInstanceMutex(L"S-1-12-1-1-2-3-5", kUser, false), kAnotherAccount);
  EXPECT_EQ(ClassifyExistingInstanceMutex(L"S-1-12-1-1-2-3-5", kUser, true), kAnotherAccount);
  EXPECT_EQ(ClassifyExistingInstanceMutex(L"", kUser, false), kAnotherAccount);
}

TEST(NamedObjects, SingleInstanceTokens) {
  EXPECT_STREQ(SingleInstanceToken(SingleInstance::kFirst), L"first");
  EXPECT_STREQ(SingleInstanceToken(SingleInstance::kOtherWindow), L"other-window");
  EXPECT_STREQ(SingleInstanceToken(SingleInstance::kAnotherAccount), L"another-account");
  EXPECT_STREQ(SingleInstanceToken(SingleInstance::kLowerIntegrity), L"lower-integrity");
}

// A window of the settings window's class counts as this user's only when its process runs as
// this user at this process's integrity level or above.
TEST(WindowMessages, OnlyThisUsersWindowAtThisIntegrityOrAbove) {
  EXPECT_TRUE(
      IsOwnWindowProcess(true, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_TRUE(IsOwnWindowProcess(true, SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_TRUE(
      IsOwnWindowProcess(true, SECURITY_MANDATORY_MEDIUM_PLUS_RID, SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_FALSE(IsOwnWindowProcess(true, SECURITY_MANDATORY_LOW_RID, SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_FALSE(
      IsOwnWindowProcess(true, SECURITY_MANDATORY_UNTRUSTED_RID, SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_FALSE(
      IsOwnWindowProcess(true, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_HIGH_RID));
  EXPECT_FALSE(
      IsOwnWindowProcess(false, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_FALSE(
      IsOwnWindowProcess(false, SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_MEDIUM_RID));
}

// CreateMutexW refuses with ERROR_ACCESS_DENIED both a mutex that another instance made with a
// descriptor that keeps this process out and every name in a namespace that this process may not
// create objects in; what opening the name and a probe in its namespace say tells them apart.
TEST(NamedObjects, RefusedCreation) {
  using enum RefusedMutex;
  // It opened to be waited on: it exists, whatever a probe would say.
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_SUCCESS, std::nullopt), kExists);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_SUCCESS, E_ACCESSDENIED), kExists);
  // Not there, or not even to be waited on: the probe decides.
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_FILE_NOT_FOUND, std::nullopt), kProbe);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_ACCESS_DENIED, std::nullopt), kProbe);
  // The probe was refused too: this process may make nothing in that namespace. Not busy.
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_FILE_NOT_FOUND, E_ACCESSDENIED), kNamespaceClosed);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_ACCESS_DENIED, E_ACCESSDENIED), kNamespaceClosed);
  // The probe was made: the mutex refuses this process even waiting on it (another instance has
  // it), or it went away between the two calls (create it again).
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_ACCESS_DENIED, S_OK), kExists);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_FILE_NOT_FOUND, S_OK), kRetry);
  // Anything else is a failure of its own, never taken for either answer.
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_INVALID_HANDLE, std::nullopt), kFailed);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_PATH_NOT_FOUND, std::nullopt), kFailed);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_INVALID_HANDLE, E_ACCESSDENIED), kFailed);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_FILE_NOT_FOUND, E_OUTOFMEMORY), kFailed);
  EXPECT_EQ(ClassifyRefusedMutex(ERROR_ACCESS_DENIED, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)),
            kFailed);
}

// The answer for a namespace closed to this process is a code of the app's own, so no Windows
// call on the way can return it, and it is neither of the lock's busy answers.
TEST(NamedObjects, RefusedIsACodeOfItsOwn) {
  EXPECT_TRUE(FAILED(kNamedObjectsRefusedHr));
  EXPECT_EQ(HRESULT_FACILITY(kNamedObjectsRefusedHr), FACILITY_ITF);
  for (const HRESULT other : {E_ACCESSDENIED, OperationLock::kBusy,
                              OperationLock::kTakenByLowerIntegrity, kNoAdministratorRightsHr}) {
    EXPECT_NE(kNamedObjectsRefusedHr, other);
  }
}

// What the commands say then: not that another check or change runs, nor to try again later.
TEST(Commands, NamedObjectsRefusedIsNotBusy) {
  const std::wstring message = NamedObjectsRefusedMessage();
  EXPECT_EQ(message.rfind(L"Windows does not let this program create named objects", 0), 0u)
      << message;
  EXPECT_NE(message.find(L"changed nothing"), std::wstring::npos) << message;
  EXPECT_EQ(message.find(L"Another check"), std::wstring::npos) << message;
  EXPECT_EQ(message.find(L"try again"), std::wstring::npos) << message;
  EXPECT_EQ(message.find(L"Cannot run here"), std::wstring::npos) << "that prefix is exit 8's";
  // Another account in someone else's session is one cause among others (a low-integrity or
  // sandboxed program meets it in its own session), so the message names it as an example.
  EXPECT_NE(message.find(L"for example"), std::wstring::npos) << message;
  EXPECT_NE(message.find(L"low integrity"), std::wstring::npos) << message;
}

// The settings executable says the same about its window, which it then does not open.
TEST(Commands, WindowNamedObjectsRefusedSaysNoWindowOpens) {
  const std::wstring message = WindowNamedObjectsRefusedMessage();
  EXPECT_EQ(message.rfind(L"Windows does not let this program create named objects", 0), 0u)
      << message;
  EXPECT_NE(message.find(L"does not open it"), std::wstring::npos) << message;
  EXPECT_EQ(message.find(L"Another"), std::wstring::npos) << message;
  EXPECT_EQ(message.find(L"try again"), std::wstring::npos) << message;
  EXPECT_NE(message.find(L"for example"), std::wstring::npos) << message;
}

}  // namespace
}  // namespace pdft
