// The pure parts of the check task's reader (tasks.h): names, paths, and what a task's triggers
// mean for the two checkboxes. Task Scheduler itself is read in the integration tests.

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
  EXPECT_EQ(DefaultTaskLocation(L"S-1-5-18\\x", &location), E_INVALIDARG);
  EXPECT_TRUE(location.folder.empty());
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

}  // namespace
}  // namespace pdft
