// --status --json (json.h): escaping, times, and the schema-1 fields of made-up snapshots, each
// output checked with a strict JSON reader (tests/support/json_check.h).

#include <windows.h>

#include "json.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "evaluate.h"
#include "json_check.h"
#include "project_config.h"
#include "snapshot_fixtures.h"
#include "strings.h"

namespace pdft {
namespace {

using namespace test;  // NOLINT(build/namespaces): the fixtures
using Type = JsonValue::Type;

JsonValue Parse(const std::string& text) {
  JsonValue value;
  std::string error;
  EXPECT_TRUE(ParseJson(text, &value, &error)) << error << "\n" << text;
  return value;
}

std::string Utf8(std::wstring_view text) { return WideToUtf8Lossy(text); }

TEST(JsonString, Escaping) {
  EXPECT_EQ(JsonString(L""), "\"\"");
  EXPECT_EQ(JsonString(L"plain"), "\"plain\"");
  EXPECT_EQ(JsonString(L"say \"hi\""), "\"say \\\"hi\\\"\"");
  EXPECT_EQ(JsonString(L"C:\\Program Files\\x"), "\"C:\\\\Program Files\\\\x\"");
  EXPECT_EQ(JsonString(L"a/b"), "\"a/b\"");
  EXPECT_EQ(JsonString(L"\b\f\n\r\t"), "\"\\b\\f\\n\\r\\t\"");
  std::wstring controls;
  controls += wchar_t{0x01};
  controls += wchar_t{0x1F};
  controls += wchar_t{0x00};
  controls += wchar_t{0x7F};  // DEL needs no escape in JSON, but a console shows it
  EXPECT_EQ(JsonString(controls), "\"\\u0001\\u001F\\u0000\\u007F\"");
}

// What a console would act on (DEL, the C1 controls, the line separators and the bidirectional
// controls) goes out escaped, so the JSON written as it is cannot steer the console.
TEST(JsonString, ConsoleControlsAreEscaped) {
  // Built from code units: the compiler warns about bidirectional characters in the source.
  const auto text = [](std::initializer_list<wchar_t> units) { return std::wstring(units); };
  EXPECT_EQ(JsonString(text({L'a', 0x7F, 0x9B, 0x2028, 0x202E, 0x2066, L'b'})),
            "\"a\\u007F\\u009B\\u2028\\u202E\\u2066b\"");
  EXPECT_EQ(JsonString(text({0x80, 0x9F, 0x2029, 0x202A, 0x2069})),
            "\"\\u0080\\u009F\\u2029\\u202A\\u2069\"");
  // Their neighbours stay UTF-8.
  EXPECT_EQ(JsonString(text({0xA0, 0x2027, 0x202F, 0x2065, 0x206A})),
            "\"\xC2\xA0\xE2\x80\xA7\xE2\x80\xAF\xE2\x81\xA5\xE2\x81\xAA\"");
}

TEST(JsonString, NonAsciiIsUtf8) {
  std::wstring text = L"caf";
  text += wchar_t{0xE9};    // e acute: C3 A9
  text += wchar_t{0x20AC};  // euro sign: E2 82 AC
  text += wchar_t{0xD83D};  // U+1F600 as a surrogate pair: F0 9F 98 80
  text += wchar_t{0xDE00};
  EXPECT_EQ(JsonString(text), "\"caf\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"");
}

TEST(JsonString, LoneSurrogatesBecomeTheReplacementCharacter) {
  const std::string replacement = "\xEF\xBF\xBD";
  EXPECT_EQ(JsonString(std::wstring(1, wchar_t{0xD800})), "\"" + replacement + "\"");
  EXPECT_EQ(JsonString(std::wstring(1, wchar_t{0xDC00})), "\"" + replacement + "\"");
  std::wstring text = L"a";
  text += wchar_t{0xDBFF};
  text += L"b";
  text += wchar_t{0xDFFF};
  text += wchar_t{0xD800};  // a high surrogate at the very end
  EXPECT_EQ(JsonString(text), "\"a" + replacement + "b" + replacement + replacement + "\"");
  // Whatever comes in, the output is valid JSON.
  JsonValue value = Parse(JsonString(text));
  EXPECT_EQ(value.type, Type::kString);
}

TEST(JsonString, RoundTripsThroughAStrictReader) {
  std::wstring text;
  for (wchar_t c = 0; c < 0x300; ++c) text += c;
  text += wchar_t{0xFFFF};
  const JsonValue value = Parse(JsonString(text));
  ASSERT_EQ(value.type, Type::kString);
  EXPECT_EQ(value.text, Utf8(text));
}

TEST(Json, IsoUtc) {
  SYSTEMTIME time = {2026, 10, 5, 30, 23, 26, 4, 512};
  FILETIME file = {};
  ASSERT_TRUE(SystemTimeToFileTime(&time, &file));
  EXPECT_EQ(IsoUtcText(file), "2026-10-30T23:26:04Z");
  EXPECT_EQ(IsoUtcText(FILETIME{0xFFFFFFFF, 0xFFFFFFFF}), "");
}

// Every field of schema 1, in order, with its JSON type.
void ExpectSchema(const JsonValue& root) {
  ASSERT_EQ(root.type, Type::kObject);
  std::vector<std::string> names;
  for (const auto& member : root.members) names.push_back(member.first);
  EXPECT_EQ(names, (std::vector<std::string>{"schema", "product", "os", "context", "install",
                                             "classes", "slots", "effective", "defaultApp",
                                             "pdfcraft", "zoom", "checks", "liveTest", "sac",
                                             "findings", "healthy", "exitCode"}));
  const auto fields = [](const JsonValue* object) {
    std::vector<std::string> keys;
    if (object) {
      for (const auto& member : object->members) keys.push_back(member.first);
    }
    return keys;
  };
  using Keys = std::vector<std::string>;
  EXPECT_EQ(root.Get("schema")->text, "1");
  EXPECT_EQ(fields(root.Get("product")), (Keys{"name", "version", "arch"}));
  EXPECT_EQ(fields(root.Get("os")), (Keys{"build", "arch"}));
  EXPECT_EQ(fields(root.Get("context")),
            (Keys{"elevated", "differentUser", "packagedAncestor", "virtualized"}));
  EXPECT_EQ(fields(root.Get("install")), (Keys{"scope", "root", "buildFolder"}));
  EXPECT_EQ(fields(root.Get("classes")), (Keys{"machine", "user"}));
  ASSERT_EQ(root.Get("slots")->type, Type::kArray);
  ASSERT_EQ(root.Get("slots")->items.size(), 2u);
  for (const JsonValue& slot : root.Get("slots")->items) {
    EXPECT_EQ(fields(&slot), (Keys{"key", "value", "ours", "name"}));
  }
  EXPECT_EQ(fields(root.Get("defaultApp")), (Keys{"progId", "handler", "overlay"}));
  EXPECT_EQ(fields(root.Get("pdfcraft")), (Keys{"installed", "version", "lastSeen"}));
  EXPECT_EQ(fields(root.Get("zoom")), (Keys{"value", "origin"}));
  EXPECT_EQ(fields(root.Get("checks")),
            (Keys{"task", "afterPdfCraft", "atSignIn", "enabled", "lastRun", "lastResult"}));
  EXPECT_EQ(fields(root.Get("liveTest")), (Keys{"ran", "hr", "ms", "skipReason"}));
  EXPECT_EQ(fields(root.Get("sac")), (Keys{"state", "verdict", "blocks", "blockLogReadable"}));
  ASSERT_EQ(root.Get("findings")->type, Type::kArray);
  for (const JsonValue& finding : root.Get("findings")->items) {
    EXPECT_EQ(fields(&finding), (Keys{"id", "severity", "breaks", "fix", "text"}));
  }
  EXPECT_EQ(root.Get("healthy")->type, Type::kBool);
  EXPECT_EQ(root.Get("exitCode")->type, Type::kNumber);
}

TEST(StatusJson, ThisPcsShape) {
  const Snapshot s = ThisPcSnapshot();
  const std::vector<Finding> findings = Evaluate(s);
  const JsonValue root = Parse(StatusJson(s, findings, ExitCodeFromFindings(findings)));
  ExpectSchema(root);
  EXPECT_EQ(root.Get("product")->Get("name")->text, Utf8(kProductName));
  EXPECT_EQ(root.Get("product")->Get("arch")->text, Utf8(kArch));
  EXPECT_EQ(root.Get("os")->Get("build")->text, "26300.9818");
  EXPECT_FALSE(root.Get("context")->Get("elevated")->boolean);
  EXPECT_TRUE(root.Get("context")->Get("packagedAncestor")->IsNull());
  EXPECT_FALSE(root.Get("context")->Get("virtualized")->boolean);
  EXPECT_EQ(root.Get("install")->Get("scope")->text, "machine");
  EXPECT_EQ(root.Get("install")->Get("root")->text, Utf8(kMachineRoot.native()));
  EXPECT_EQ(root.Get("install")->Get("buildFolder")->text, "1.1.0.0-67a1323e");
  const JsonValue* machine = root.Get("classes")->Get("machine");
  ASSERT_EQ(machine->type, Type::kObject);
  EXPECT_EQ(machine->Get("dll")->text, Utf8(s.machineClass->dll));
  EXPECT_TRUE(machine->Get("dllExists")->boolean);
  EXPECT_EQ(machine->Get("threadingModel")->text, "Apartment");
  EXPECT_EQ(machine->Get("version")->text, "1.1.0.0");
  EXPECT_EQ(machine->Get("arch")->text, Utf8(kNativeArchName));
  EXPECT_TRUE(root.Get("classes")->Get("user")->IsNull());
  const JsonValue& slot = root.Get("slots")->items[0];
  EXPECT_EQ(slot.Get("value")->text, Utf8(kOurs));
  EXPECT_TRUE(slot.Get("ours")->boolean);
  EXPECT_EQ(slot.Get("name")->text, Utf8(kProductName));
  EXPECT_EQ(slot.Get("key")->text, Utf8(SlotKeyPath(0)));
  EXPECT_EQ(root.Get("effective")->text, Utf8(kOurs));
  EXPECT_EQ(root.Get("defaultApp")->Get("progId")->text, "PdfCraft.Document");
  EXPECT_TRUE(root.Get("defaultApp")->Get("handler")->IsNull());
  EXPECT_EQ(root.Get("defaultApp")->Get("overlay")->type, Type::kString);
  EXPECT_EQ(root.Get("defaultApp")->Get("overlay")->text, "");
  EXPECT_TRUE(root.Get("pdfcraft")->Get("installed")->boolean);
  EXPECT_EQ(root.Get("pdfcraft")->Get("version")->text, "0.14.0");
  EXPECT_TRUE(root.Get("pdfcraft")->Get("lastSeen")->IsNull());
  EXPECT_EQ(root.Get("zoom")->Get("value")->text, "130");
  EXPECT_EQ(root.Get("zoom")->Get("origin")->text, "legacyIni");
  EXPECT_TRUE(root.Get("checks")->Get("task")->IsNull());
  EXPECT_FALSE(root.Get("checks")->Get("enabled")->boolean);
  EXPECT_TRUE(root.Get("checks")->Get("lastRun")->IsNull());
  EXPECT_TRUE(root.Get("liveTest")->Get("ran")->boolean);
  EXPECT_EQ(root.Get("liveTest")->Get("hr")->text, "0x00000000");
  EXPECT_EQ(root.Get("liveTest")->Get("ms")->text, "212");
  EXPECT_TRUE(root.Get("liveTest")->Get("skipReason")->IsNull());
  EXPECT_EQ(root.Get("sac")->Get("state")->text, "on");
  EXPECT_EQ(root.Get("sac")->Get("verdict")->text, "ok");
  EXPECT_TRUE(root.Get("sac")->Get("blocks")->IsNull());
  EXPECT_TRUE(root.Get("sac")->Get("blockLogReadable")->IsNull());
  std::vector<std::string> ids;
  for (const JsonValue& finding : root.Get("findings")->items) {
    ids.push_back(finding.Get("id")->text);
    EXPECT_FALSE(finding.Get("text")->text.empty());
  }
  EXPECT_EQ(ids, (std::vector<std::string>{"SacOn", "ChecksOff", "V1Leftovers"}));
  EXPECT_EQ(root.Get("findings")->items[2].Get("severity")->text, "info");
  EXPECT_EQ(root.Get("findings")->items[2].Get("fix")->text, "none");
  EXPECT_FALSE(root.Get("findings")->items[2].Get("breaks")->boolean);
  EXPECT_TRUE(root.Get("healthy")->boolean);
  EXPECT_EQ(root.Get("exitCode")->text, "0");
}

TEST(StatusJson, NothingInstalledAndSkippedChecks) {
  Snapshot s = EmptySnapshot();
  s.context.packagedAncestor = L"Claude_1.0.0.0_x64__abc";
  s.context.views = RegistryViews::kDiffer;
  s.windowsChecksSkipped = true;
  s.windowsChecksSkip = WindowsChecksSkip::kViewsDiffer;
  s.liveSkip = LiveTestSkip::kVirtualized;
  s.osBuild.clear();
  const std::vector<Finding> findings = Evaluate(s);
  const JsonValue root = Parse(StatusJson(s, findings, ExitCode::kNotInstalled));
  ExpectSchema(root);
  EXPECT_TRUE(root.Get("os")->Get("build")->IsNull());
  EXPECT_EQ(root.Get("context")->Get("packagedAncestor")->text, "Claude_1.0.0.0_x64__abc");
  EXPECT_TRUE(root.Get("context")->Get("virtualized")->boolean);
  EXPECT_EQ(root.Get("install")->Get("scope")->text, "none");
  EXPECT_TRUE(root.Get("install")->Get("root")->IsNull());
  EXPECT_TRUE(root.Get("install")->Get("buildFolder")->IsNull());
  EXPECT_TRUE(root.Get("classes")->Get("machine")->IsNull());
  EXPECT_TRUE(root.Get("slots")->items[0].Get("value")->IsNull());
  EXPECT_FALSE(root.Get("slots")->items[0].Get("ours")->boolean);
  EXPECT_TRUE(root.Get("effective")->IsNull());
  EXPECT_FALSE(root.Get("liveTest")->Get("ran")->boolean);
  EXPECT_TRUE(root.Get("liveTest")->Get("hr")->IsNull());
  EXPECT_EQ(root.Get("liveTest")->Get("skipReason")->text, "virtualized");
  EXPECT_EQ(root.Get("sac")->Get("state")->text, "absent");
  EXPECT_EQ(root.Get("sac")->Get("verdict")->text, "none");
  EXPECT_EQ(root.Get("findings")->items[0].Get("id")->text, "NotInstalled");
  EXPECT_EQ(root.Get("findings")->items[0].Get("severity")->text, "error");
  EXPECT_TRUE(root.Get("findings")->items[0].Get("breaks")->boolean);
  EXPECT_EQ(root.Get("findings")->items[0].Get("fix")->text, "userAction");
  EXPECT_FALSE(root.Get("healthy")->boolean);
  EXPECT_EQ(root.Get("exitCode")->text, "6");
}

TEST(StatusJson, ProblemsTaskBlocksAndOddStrings) {
  Snapshot s = UserInstallSnapshot();
  // Strings anyone could have written into the registry.
  std::wstring odd = L"{\"quoted\"}\\";
  odd += wchar_t{0x01};
  odd += wchar_t{0xD800};
  odd += L"\n";
  s.slots = {odd, std::nullopt};
  s.slotOwners[0].name = odd;
  s.defaultApp.progId = odd;
  s.task.lastResult = static_cast<LONG>(0x80070002);
  SYSTEMTIME local = {2026, 10, 5, 30, 1, 26, 0, 0};
  DATE date = 0;
  ASSERT_TRUE(SystemTimeToVariantTime(&local, &date));
  s.task.lastRun = date;
  s.live = LiveTest{static_cast<HRESULT>(0x800711C7), {}, 50};
  s.sac.state = SacState::kOn;
  s.blocks = BlockCount{true, 2, S_OK};
  s.sacVerdict = DecideSac(SacState::kOn, s.live->hr, s.blocks);
  const std::vector<Finding> findings = Evaluate(s);
  const JsonValue root = Parse(StatusJson(s, findings, ExitCodeFromFindings(findings)));
  ExpectSchema(root);
  const JsonValue& slot = root.Get("slots")->items[0];
  EXPECT_FALSE(slot.Get("ours")->boolean);
  EXPECT_EQ(slot.Get("value")->text, Utf8(odd));
  EXPECT_EQ(slot.Get("name")->text, Utf8(odd));
  EXPECT_EQ(root.Get("checks")->Get("task")->text, Utf8(s.task.path));
  EXPECT_TRUE(root.Get("checks")->Get("afterPdfCraft")->boolean);
  EXPECT_EQ(root.Get("checks")->Get("lastResult")->text, "0x80070002");
  const std::string lastRun = root.Get("checks")->Get("lastRun")->text;
  ASSERT_EQ(lastRun.size(), 20u) << lastRun;
  EXPECT_EQ(lastRun.back(), 'Z');
  EXPECT_EQ(root.Get("liveTest")->Get("hr")->text, "0x800711C7");
  EXPECT_EQ(root.Get("sac")->Get("verdict")->text, "blocked");
  EXPECT_EQ(root.Get("sac")->Get("blocks")->text, "2");
  EXPECT_TRUE(root.Get("sac")->Get("blockLogReadable")->boolean);
  EXPECT_FALSE(root.Get("healthy")->boolean);
  EXPECT_EQ(root.Get("exitCode")->text, "1");
  bool sawSlot = false;
  for (const JsonValue& finding : root.Get("findings")->items) {
    if (finding.Get("id")->text == "SlotOtherHandler") {
      sawSlot = true;
      EXPECT_EQ(finding.Get("fix")->text, "takeBack");
    }
  }
  EXPECT_TRUE(sawSlot);

  // An unreadable Code Integrity log.
  s.blocks = BlockCount{false, 0, E_ACCESSDENIED};
  const JsonValue unreadable = Parse(StatusJson(s, Evaluate(s), ExitCode::kProblems));
  EXPECT_TRUE(unreadable.Get("sac")->Get("blocks")->IsNull());
  EXPECT_FALSE(unreadable.Get("sac")->Get("blockLogReadable")->boolean);
}

TEST(JsonCheck, RejectsWhatIsNotJson) {
  JsonValue value;
  for (const char* bad :
       {"", "{", "{\"a\":1,}", "[1,]", "{'a':1}", "\"\x01\"", "01", "1.", "\"\\x\"", "\"\\uD800\"",
        "\"\xC3\"", "\"\xED\xA0\x80\"", "{} x", "nul", "[1 2]"}) {
    EXPECT_FALSE(ParseJson(bad, &value)) << bad;
  }
  for (const char* good : {"{}", "[]", "0", "-1.5e+3", "\"\\uD83D\\uDE00\"", "null",
                           " {\"a\" : [true, false, null]} "}) {
    EXPECT_TRUE(ParseJson(good, &value)) << good;
  }
}

}  // namespace
}  // namespace pdft
