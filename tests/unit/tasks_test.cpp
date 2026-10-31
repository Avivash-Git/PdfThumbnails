// The pure parts of the check task's module (tasks.h): names, paths, what a task's triggers mean
// for the two checkboxes, the triggers it registers, and which task folders can be trusted with a
// user's task. Task Scheduler itself is used in the integration tests.

#include <windows.h>

#include "tasks.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "project_config.h"

namespace pdft {
namespace {

// A made-up user SID in the Microsoft Entra form (S-1-12-1-...): the repository's publish check
// rejects anything shaped like a local or domain account's SID, even an invented one.
constexpr wchar_t kSid[] = L"S-1-12-1-1-2-3-4";

TriggerFacts Logon(bool enabled = true) {
  TriggerFacts trigger;
  trigger.type = TASK_TRIGGER_LOGON;
  trigger.enabled = enabled;
  return trigger;
}

TriggerFacts Event(const std::wstring& subscription, bool enabled = true) {
  TriggerFacts trigger;
  trigger.type = TASK_TRIGGER_EVENT;
  trigger.enabled = enabled;
  trigger.subscription = subscription;
  return trigger;
}

TriggerFacts OfType(TASK_TRIGGER_TYPE2 type) {
  TriggerFacts trigger;
  trigger.type = type;
  return trigger;
}

TEST(TaskNames, Sids) {
  EXPECT_TRUE(IsSidString(kSid));
  EXPECT_TRUE(IsSidString(L"S-1-5-18"));
  for (const wchar_t* bad : {L"", L"S-1-", L"S-1-5-", L"S-1--5", L"s-1-5-18", L"S-2-5-18",
                             L"S-1-5-18\\..", L"S-1-5-18 ", L"..\\S-1-5-18", L"S-1-5-1a"}) {
    EXPECT_FALSE(IsSidString(bad)) << bad;
  }
  EXPECT_FALSE(IsSidString(L"S-1-" + std::wstring(181, L'1')));
}

TEST(TaskNames, DefaultLocation) {
  TaskLocation location;
  ASSERT_HRESULT_SUCCEEDED(DefaultTaskLocation(kSid, &location));
  EXPECT_EQ(location.folder, std::wstring(L"\\") + kProductName);
  EXPECT_EQ(location.name, std::wstring(L"Check ") + kSid);
  EXPECT_EQ(location.fallbackFolder, L"\\");
  EXPECT_EQ(location.fallbackName, std::wstring(kProductName) + L" check " + kSid);
  EXPECT_EQ(location.userSid, kSid);
  EXPECT_EQ(DefaultTaskLocation(L"S-1-5-18\\x", &location), E_INVALIDARG);
  EXPECT_TRUE(location.folder.empty());
  EXPECT_TRUE(location.userSid.empty());
}

// Whose task it is: the principal's UserId, as Task Scheduler's XML holds it.
TEST(TaskNames, PrincipalUser) {
  const std::wstring xml =
      L"<?xml version=\"1.0\" encoding=\"UTF-16\"?><Task version=\"1.4\" "
      L"xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"><Triggers><LogonTrigger>"
      L"<UserId>MACHINE\\someone</UserId></LogonTrigger></Triggers><Principals>\r\n"
      L"    <Principal id=\"Author\">\r\n      <UserId> " +
      std::wstring(kSid) +
      L"\r\n</UserId>\r\n      <LogonType>InteractiveToken</LogonType>\r\n    </Principal>\r\n"
      L"  </Principals><Actions><Exec><Command>C:\\x.exe</Command></Exec></Actions></Task>";
  // The logon trigger's user comes first in the XML but is not the principal.
  EXPECT_EQ(TaskPrincipalUserId(xml), kSid);
  // A group principal, no principal, or no XML at all names no user.
  EXPECT_EQ(TaskPrincipalUserId(L"<Task><Principals><Principal id=\"Author\"><GroupId>S-1-5-32-545"
                                L"</GroupId></Principal></Principals><Actions><Exec><UserId>x"
                                L"</UserId></Exec></Actions></Task>"),
            L"");
  EXPECT_EQ(TaskPrincipalUserId(L"<Task><Triggers><LogonTrigger><UserId>x</UserId>"
                                L"</LogonTrigger></Triggers></Task>"),
            L"");
  EXPECT_EQ(TaskPrincipalUserId(L""), L"");
  EXPECT_EQ(TaskPrincipalUserId(L"<Principals><UserId>S-1-5-18"), L"");  // cut off
}

TEST(TaskNames, SplitPath) {
  std::wstring folder;
  std::wstring name;
  ASSERT_TRUE(SplitTaskPath(kLegacyTask, &folder, &name));
  EXPECT_EQ(folder, L"\\");
  EXPECT_EQ(name, L"PdfThumbnails auto-repair");
  ASSERT_TRUE(SplitTaskPath(L"\\PdfThumbnails\\Check S-1-5-18", &folder, &name));
  EXPECT_EQ(folder, L"\\PdfThumbnails");
  EXPECT_EQ(name, L"Check S-1-5-18");
  ASSERT_TRUE(SplitTaskPath(L"\\a\\b\\c", &folder, &name));
  EXPECT_EQ(folder, L"\\a\\b");
  EXPECT_EQ(name, L"c");
  for (const wchar_t* bad : {L"", L"\\", L"name", L"\\folder\\", L"folder\\name"}) {
    EXPECT_FALSE(SplitTaskPath(bad, &folder, &name)) << bad;
  }
}

TEST(TaskNames, NotFound) {
  EXPECT_TRUE(IsTaskNotFound(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)));
  EXPECT_TRUE(IsTaskNotFound(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)));
  EXPECT_FALSE(IsTaskNotFound(E_ACCESSDENIED));
  EXPECT_FALSE(IsTaskNotFound(S_OK));
  EXPECT_FALSE(IsTaskNotFound(S_FALSE));
}

TEST(TaskTriggerClassification, PdfCraftSubscription) {
  const std::wstring exact = kPdfCraftSubscription;
  EXPECT_TRUE(IsPdfCraftSubscription(exact));
  // As 1.x registered it, character for character.
  EXPECT_TRUE(IsPdfCraftSubscription(
      L"<QueryList><Query Id=\"0\" Path=\"Application\"><Select Path=\"Application\">*[System["
      L"Provider[@Name='MsiInstaller'] and EventID=1033] and EventData[Data='PdfCraft']]"
      L"</Select></Query></QueryList>"));
  // Pretty-printed XML is the same query.
  EXPECT_TRUE(IsPdfCraftSubscription(
      L"\r\n  <QueryList>\r\n    <Query Id=\"0\" Path=\"Application\">\r\n      <Select "
      L"Path=\"Application\">*[System[Provider[@Name='MsiInstaller'] and EventID=1033] and "
      L"EventData[Data='PdfCraft']]</Select>\r\n    </Query>\r\n  </QueryList>\r\n"));
  // Another product, another event, another case, space inside the query: not ours.
  std::wstring other = exact;
  other.replace(other.find(L"PdfCraft"), 8, L"OtherApp");
  EXPECT_FALSE(IsPdfCraftSubscription(other));
  other = exact;
  other.replace(other.find(L"1033"), 4, L"1034");
  EXPECT_FALSE(IsPdfCraftSubscription(other));
  other = exact;
  other.replace(other.find(L"PdfCraft"), 8, L"pdfcraft");
  EXPECT_FALSE(IsPdfCraftSubscription(other));
  other = exact;
  other.replace(other.find(L" and EventID"), 1, L"  ");
  EXPECT_FALSE(IsPdfCraftSubscription(other));
  EXPECT_FALSE(IsPdfCraftSubscription(L""));
}

TEST(TaskTriggerClassification, Checkboxes) {
  const std::wstring pdfcraft = kPdfCraftSubscription;
  {
    const std::vector<TriggerFacts> both = {Logon(), Event(pdfcraft)};
    const TaskTriggers result = ClassifyTriggers(both);
    EXPECT_TRUE(result.afterPdfCraft);
    EXPECT_TRUE(result.atSignIn);
  }
  {
    const std::vector<TriggerFacts> signIn = {Logon()};
    const TaskTriggers result = ClassifyTriggers(signIn);
    EXPECT_FALSE(result.afterPdfCraft);
    EXPECT_TRUE(result.atSignIn);
  }
  {
    const std::vector<TriggerFacts> afterPdfCraft = {Event(pdfcraft)};
    const TaskTriggers result = ClassifyTriggers(afterPdfCraft);
    EXPECT_TRUE(result.afterPdfCraft);
    EXPECT_FALSE(result.atSignIn);
  }
  {
    // Disabled triggers never fire.
    const std::vector<TriggerFacts> disabled = {Logon(false), Event(pdfcraft, false)};
    const TaskTriggers result = ClassifyTriggers(disabled);
    EXPECT_FALSE(result.afterPdfCraft);
    EXPECT_FALSE(result.atSignIn);
  }
  {
    // Another event, and kinds of triggers the checkboxes do not stand for.
    const std::vector<TriggerFacts> others = {
        Event(L"<QueryList><Query Id=\"0\" Path=\"System\"><Select Path=\"System\">*</Select>"
              L"</Query></QueryList>"),
        OfType(TASK_TRIGGER_BOOT), OfType(TASK_TRIGGER_DAILY),
        OfType(TASK_TRIGGER_SESSION_STATE_CHANGE), OfType(TASK_TRIGGER_REGISTRATION)};
    const TaskTriggers result = ClassifyTriggers(others);
    EXPECT_FALSE(result.afterPdfCraft);
    EXPECT_FALSE(result.atSignIn);
  }
  {
    // An event trigger whose subscription could not be read is not PdfCraft's.
    const std::vector<TriggerFacts> empty = {Event(L"")};
    EXPECT_FALSE(ClassifyTriggers(empty).afterPdfCraft);
  }
  EXPECT_FALSE(ClassifyTriggers({}).atSignIn);
}

// ---- Writing ------------------------------------------------------------------------------

TEST(TaskOptionsTest, FromCheckboxes) {
  for (const bool afterPdfCraft : {false, true}) {
    for (const bool atSignIn : {false, true}) {
      const TaskOptions options = OptionsFromCheckboxes(afterPdfCraft, atSignIn);
      EXPECT_EQ(options.afterPdfCraft,
                afterPdfCraft ? TriggerSetting::kEnabled : TriggerSetting::kAbsent);
      EXPECT_EQ(options.atSignIn, atSignIn ? TriggerSetting::kEnabled : TriggerSetting::kAbsent);
      EXPECT_TRUE(options.enabled);
      EXPECT_EQ(HasNoTriggers(options), !afterPdfCraft && !atSignIn);
    }
  }
  // A disabled trigger is still a trigger: the task stays.
  EXPECT_FALSE(HasNoTriggers({TriggerSetting::kDisabled, TriggerSetting::kAbsent, true}));
  EXPECT_FALSE(HasNoTriggers({TriggerSetting::kAbsent, TriggerSetting::kDisabled, false}));
  // The defaults are both triggers, enabled.
  const TaskOptions defaults;
  EXPECT_EQ(defaults.afterPdfCraft, TriggerSetting::kEnabled);
  EXPECT_EQ(defaults.atSignIn, TriggerSetting::kEnabled);
  EXPECT_TRUE(defaults.enabled);
  // No task: no triggers, not enabled.
  const TaskState none;
  EXPECT_EQ(none.options.afterPdfCraft, TriggerSetting::kAbsent);
  EXPECT_EQ(none.options.atSignIn, TriggerSetting::kAbsent);
  EXPECT_FALSE(none.options.enabled);
}

TEST(TaskOptionsTest, SettingsOfRegisteredTriggers) {
  const std::wstring pdfcraft = kPdfCraftSubscription;
  {
    const std::vector<TriggerFacts> both = {Logon(), Event(pdfcraft)};
    const TaskOptions options = TriggerSettingsOf(both);
    EXPECT_EQ(options.atSignIn, TriggerSetting::kEnabled);
    EXPECT_EQ(options.afterPdfCraft, TriggerSetting::kEnabled);
    EXPECT_TRUE(options.enabled);
  }
  {
    // Disabled triggers are told apart from missing ones.
    const std::vector<TriggerFacts> disabled = {Logon(false), Event(pdfcraft, false)};
    const TaskOptions options = TriggerSettingsOf(disabled);
    EXPECT_EQ(options.atSignIn, TriggerSetting::kDisabled);
    EXPECT_EQ(options.afterPdfCraft, TriggerSetting::kDisabled);
  }
  {
    // An enabled trigger of a kind wins over a disabled one of the same kind, in either order.
    const std::vector<TriggerFacts> mixed = {Logon(false), Logon(), Event(pdfcraft),
                                             Event(pdfcraft, false)};
    const TaskOptions options = TriggerSettingsOf(mixed);
    EXPECT_EQ(options.atSignIn, TriggerSetting::kEnabled);
    EXPECT_EQ(options.afterPdfCraft, TriggerSetting::kEnabled);
  }
  {
    // Another event and other kinds of triggers stand for neither checkbox.
    const std::vector<TriggerFacts> others = {
        Event(L"<QueryList><Query Id=\"0\" Path=\"System\"><Select Path=\"System\">*</Select>"
              L"</Query></QueryList>"),
        Event(L"", false), OfType(TASK_TRIGGER_BOOT), OfType(TASK_TRIGGER_DAILY)};
    const TaskOptions options = TriggerSettingsOf(others);
    EXPECT_EQ(options.atSignIn, TriggerSetting::kAbsent);
    EXPECT_EQ(options.afterPdfCraft, TriggerSetting::kAbsent);
  }
  // The checkbox states agree with the settings: only enabled triggers count.
  const std::vector<TriggerFacts> some = {Logon(false), Event(pdfcraft)};
  const TaskTriggers boxes = ClassifyTriggers(some);
  const TaskOptions options = TriggerSettingsOf(some);
  EXPECT_EQ(boxes.atSignIn, options.atSignIn == TriggerSetting::kEnabled);
  EXPECT_EQ(boxes.afterPdfCraft, options.afterPdfCraft == TriggerSetting::kEnabled);
}

TEST(TaskOptionsTest, TriggerSpecs) {
  constexpr wchar_t kAccount[] = L"EXAMPLE\\someone";
  const std::vector<TriggerSpec> both =
      BuildTriggerSpecs(OptionsFromCheckboxes(true, true), kAccount);
  ASSERT_EQ(both.size(), 2u);
  // The logon trigger first, as 1.x listed them.
  EXPECT_EQ(both[0].type, TASK_TRIGGER_LOGON);
  EXPECT_EQ(both[0].id, L"AtSignIn");
  EXPECT_EQ(both[0].delay, L"PT1M");
  EXPECT_EQ(both[0].userId, kAccount);
  EXPECT_TRUE(both[0].subscription.empty());
  EXPECT_TRUE(both[0].enabled);
  EXPECT_EQ(both[1].type, TASK_TRIGGER_EVENT);
  EXPECT_EQ(both[1].id, L"AfterPdfCraftInstall");
  EXPECT_EQ(both[1].delay, L"PT30S");
  EXPECT_EQ(both[1].subscription, kPdfCraftSubscription);  // character for character
  EXPECT_TRUE(both[1].userId.empty());
  EXPECT_TRUE(both[1].enabled);

  const std::vector<TriggerSpec> signIn =
      BuildTriggerSpecs(OptionsFromCheckboxes(false, true), kAccount);
  ASSERT_EQ(signIn.size(), 1u);
  EXPECT_EQ(signIn[0].type, TASK_TRIGGER_LOGON);
  const std::vector<TriggerSpec> afterPdfCraft =
      BuildTriggerSpecs(OptionsFromCheckboxes(true, false), kAccount);
  ASSERT_EQ(afterPdfCraft.size(), 1u);
  EXPECT_EQ(afterPdfCraft[0].type, TASK_TRIGGER_EVENT);
  EXPECT_TRUE(BuildTriggerSpecs(OptionsFromCheckboxes(false, false), kAccount).empty());

  // Disabled triggers are registered disabled.
  const std::vector<TriggerSpec> disabled =
      BuildTriggerSpecs({TriggerSetting::kDisabled, TriggerSetting::kDisabled, false}, kSid);
  ASSERT_EQ(disabled.size(), 2u);
  EXPECT_FALSE(disabled[0].enabled);
  EXPECT_FALSE(disabled[1].enabled);
  EXPECT_EQ(disabled[0].userId, kSid);

  // What the registered task says about itself.
  EXPECT_NE(TaskDescription().find(kProductName), std::wstring::npos);
  EXPECT_NE(TaskDescription().find(kPdfCraftProductName), std::wstring::npos);
  // "Some triggers failed" as a failure: the same code, with the failure bit.
  EXPECT_TRUE(FAILED(kSomeTriggersFailed));
  EXPECT_EQ(HRESULT_CODE(kSomeTriggersFailed), HRESULT_CODE(SCHED_S_SOME_TRIGGERS_FAILED));
  EXPECT_EQ(HRESULT_FACILITY(kSomeTriggersFailed), HRESULT_FACILITY(SCHED_S_SOME_TRIGGERS_FAILED));
}

// The DACL of kTaskFolderSddl, to build variants of it.
constexpr wchar_t kFolderDacl[] =
    L"D:P(A;CI;FA;;;BA)(A;OI;0x1f019f;;;BA)(A;CI;FA;;;SY)(A;OI;0x1f019f;;;SY)(A;;FW;;;AU)"
    L"(A;OIIO;FA;;;CO)";

TEST(TaskFolderTrust, TheFolderDescriptor) {
  EXPECT_TRUE(IsTrustedTaskFolderSddl(kTaskFolderSddl));
  EXPECT_EQ(std::wstring(kTaskFolderSddl), std::wstring(L"O:BAG:SY") + kFolderDacl);
  // Owned by SYSTEM (made by an installer that runs as SYSTEM), without a group, and with the
  // inheritance flag Task Scheduler may add to the DACL.
  EXPECT_TRUE(IsTrustedTaskFolderSddl(std::wstring(L"O:SYG:SY") + kFolderDacl));
  EXPECT_TRUE(IsTrustedTaskFolderSddl(std::wstring(L"O:BA") + kFolderDacl));
  EXPECT_TRUE(IsTrustedTaskFolderSddl(
      L"O:BAG:SYD:PAI(A;CI;FA;;;BA)(A;OI;0x1f019f;;;BA)(A;CI;FA;;;SY)(A;OI;0x1f019f;;;SY)"
      L"(A;;FW;;;AU)(A;OIIO;FA;;;CO)"));
  // Fewer rights for users, a deny entry, and the group do not matter.
  EXPECT_TRUE(IsTrustedTaskFolderSddl(L"O:BAD:P(A;;FA;;;BA)(A;;0x2;;;AU)"));
  EXPECT_TRUE(
      IsTrustedTaskFolderSddl(std::wstring(L"O:BAG:BU") + kFolderDacl + L"(D;OICI;FA;;;WD)"));
  EXPECT_TRUE(IsTrustedTaskFolderSddl(L"O:BAD:P(A;;FA;;;BA)"));
}

TEST(TaskFolderTrust, TheRootFolderAndFoldersUsersMade) {
  // The root folder, as Windows sets it up: the users' right is inherited by subfolders, and the
  // creator of a subfolder gets full control of it.
  EXPECT_FALSE(IsTrustedTaskFolderSddl(
      L"O:SYG:SYD:PAI(A;CI;FA;;;BA)(A;OI;0x1f019f;;;BA)(A;CI;FA;;;SY)(A;OI;0x1f019f;;;SY)"
      L"(A;CI;FW;;;AU)(A;CI;FW;;;NS)(A;CI;FW;;;LS)(A;OICIIO;FA;;;CO)"));
  // A folder a standard user made: they own it and hold full control of it.
  const std::wstring user = kSid;
  EXPECT_FALSE(IsTrustedTaskFolderSddl(
      L"O:" + user + L"G:" + user +
      L"D:AI(A;CIID;FA;;;BA)(A;OIIOID;0x1f019f;;;BA)(A;CIID;FA;;;SY)(A;OIIOID;0x1f019f;;;SY)"
      L"(A;CIID;FW;;;AU)(A;CIID;FW;;;NS)(A;CIID;FW;;;LS)(A;ID;FA;;;" +
      user + L")(A;OICIIOID;FA;;;CO)"));
  // The safe DACL is not enough with another owner, who may rewrite it.
  EXPECT_FALSE(IsTrustedTaskFolderSddl(L"O:" + user + kFolderDacl));
  EXPECT_FALSE(IsTrustedTaskFolderSddl(std::wstring(L"O:AU") + kFolderDacl));
  EXPECT_FALSE(IsTrustedTaskFolderSddl(std::wstring(L"O:BU") + kFolderDacl));
  // No owner at all.
  EXPECT_FALSE(IsTrustedTaskFolderSddl(kFolderDacl));
}

TEST(TaskFolderTrust, RightsBeyondAddingTasks) {
  const std::wstring start = L"O:BAG:SYD:P(A;CI;FA;;;BA)(A;CI;FA;;;SY)";
  for (const wchar_t* ace : {
           L"(A;;FA;;;AU)",      // full control
           L"(A;;FR;;;AU)",      // listing and reading
           L"(A;;GA;;;AU)",      // generic rights
           L"(A;;GW;;;AU)",      //
           L"(A;;SD;;;AU)",      // deleting the folder
           L"(A;;WD;;;AU)",      // rewriting its permissions
           L"(A;;WO;;;AU)",      // taking it over
           L"(A;;0x40;;;AU)",    // deleting what is in it
           L"(A;CI;FW;;;AU)",    // adding, but inherited by subfolders
           L"(A;OI;FW;;;AU)",    // ... by tasks
           L"(A;CIIO;FW;;;AU)",  //
           L"(A;;FW;;;WD)(A;;FR;;;WD)",
           L"(A;;0x2;;;S-1-12-1-1-2-3-4)(A;OI;FR;;;S-1-12-1-1-2-3-4)",
           L"(A;;FA;;;CO)",  // creator-owner on the folder itself
           L"(A;OI;FA;;;CO)",
           L"(OA;;FA;bf967aba-0de6-11d0-a285-00aa003049e2;;AU)",  // an object entry
           L"(XA;;FA;;;AU;(Member_of {SID(BA)}))",                // a conditional entry
       }) {
    EXPECT_FALSE(IsTrustedTaskFolderSddl(start + ace)) << ace;
  }
  // Adding tasks, on the folder only, is fine for anyone, and so is the creator-owner entry.
  for (const wchar_t* ace : {L"(A;;FW;;;AU)", L"(A;;FW;;;WD)", L"(A;;FW;;;BU)", L"(A;;0x2;;;IU)",
                             L"(A;OIIO;FA;;;CO)", L"(A;OICIIO;FA;;;CO)", L"(D;;FA;;;AN)"}) {
    EXPECT_TRUE(IsTrustedTaskFolderSddl(start + ace)) << ace;
  }
}

TEST(TaskFolderTrust, WhatCannotBeJudged) {
  EXPECT_FALSE(IsTrustedTaskFolderSddl(L""));
  EXPECT_FALSE(IsTrustedTaskFolderSddl(L"not a descriptor"));
  EXPECT_FALSE(IsTrustedTaskFolderSddl(L"O:BA"));                     // no DACL
  EXPECT_FALSE(IsTrustedTaskFolderSddl(L"O:BAD:NO_ACCESS_CONTROL"));  // everyone may do all
  EXPECT_FALSE(IsTrustedTaskFolderSddl(L"O:BAD:(A;;FA;;;BA"));        // cut off
  // An empty DACL lets nobody in: nothing to fear and nothing to use (registering fails then).
  EXPECT_TRUE(IsTrustedTaskFolderSddl(L"O:BAD:P"));
}

TEST(TaskFolderTrust, Place) {
  EXPECT_EQ(ChooseTaskPlace(true, kTaskFolderSddl), TaskPlace::kFolder);
  EXPECT_EQ(ChooseTaskPlace(false, kTaskFolderSddl), TaskPlace::kFallback);
  EXPECT_EQ(ChooseTaskPlace(true, L""), TaskPlace::kFallback);  // the descriptor was unreadable
  EXPECT_EQ(ChooseTaskPlace(true, std::wstring(L"O:") + kSid + kFolderDacl), TaskPlace::kFallback);
  EXPECT_EQ(ChooseTaskPlace(false, L""), TaskPlace::kFallback);
}

TEST(TaskFolderTrust, TopLevelFolders) {
  EXPECT_TRUE(IsTopLevelTaskFolder(kTaskFolder));
  EXPECT_TRUE(IsTopLevelTaskFolder(L"\\PdfThumbnails-Test-1"));
  for (const wchar_t* bad :
       {L"", L"\\", L"PdfThumbnails", L"\\a\\b", L"\\a\\", L"\\a/b", L"\\.", L"\\.."}) {
    EXPECT_FALSE(IsTopLevelTaskFolder(bad)) << bad;
  }
}

}  // namespace
}  // namespace pdft
