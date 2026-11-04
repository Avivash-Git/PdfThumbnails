// The command line (cli_args.h): every command and option, every accepted form, the help
// spellings, each kind of usage error, and which commands write.

#include "cli_args.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "project_config.h"

namespace pdft {
namespace {

constexpr ParseOptions kCli{Mode::kCli, true};
constexpr ParseOptions kGui{Mode::kGui, true};
constexpr ParseOptions kCliRelease{Mode::kCli, false};
constexpr ParseOptions kGuiRelease{Mode::kGui, false};

std::variant<CliArgs, ParseError> Parse(const ParseOptions& options,
                                        std::initializer_list<std::wstring_view> words) {
  const std::vector<std::wstring_view> args(words);
  return ParseCommandLine(args, options);
}

// One row: the arguments, and either the command and its writes flag, or a piece of the error.
struct Case {
  const char* name;
  ParseOptions options;
  std::vector<std::wstring_view> args;
  bool ok;
  Verb verb = Verb::kNone;
  bool writes = false;
  std::wstring_view error = {};
};

Case Ok(const char* name, const ParseOptions& options, std::vector<std::wstring_view> args,
        Verb verb, bool writes) {
  return {name, options, std::move(args), true, verb, writes};
}

Case Fails(const char* name, const ParseOptions& options, std::vector<std::wstring_view> args,
           std::wstring_view error) {
  return {name, options, std::move(args), false, Verb::kNone, false, error};
}

const std::vector<Case>& Cases() {
  using enum Verb;
  static const std::vector<Case> cases = {
      // No command, the help spellings, --version.
      Ok("NoArgumentsCli", kCli, {}, kNone, false),
      Ok("NoArgumentsGui", kGui, {}, kNone, false),
      Ok("RelaunchedAloneGui", kGui, {L"--relaunched"}, kNone, false),
      Ok("Help", kCli, {L"--help"}, kHelp, false),
      Ok("HelpDashH", kCli, {L"-h"}, kHelp, false),
      Ok("HelpDashQuestion", kCli, {L"-?"}, kHelp, false),
      Ok("HelpSlashQuestion", kGui, {L"/?"}, kHelp, false),
      Ok("Version", kCli, {L"--version"}, kVersion, false),
      Ok("VersionGui", kGui, {L"--version"}, kVersion, false),
      // Each command with its options, in each value form.
      Ok("Status", kCli, {L"--status"}, kStatus, false),
      Ok("StatusAllOptions", kCli,
         {L"--status", L"--json", L"--no-live-test", L"--test-file", L"a.pdf"}, kStatus, false),
      Ok("StatusTestFileEquals", kCli, {L"--status", L"--test-file=C:\\x y\\a.pdf"}, kStatus,
         false),
      Ok("StatusRelaunchedGui", kGui, {L"--status", L"--relaunched"}, kStatus, false),
      Ok("Check", kCli, {L"--check"}, kCheck, true),
      Ok("CheckAllOptions", kCli, {L"--check", L"--quiet", L"--no-repair", L"--no-live-test"},
         kCheck, true),
      Ok("Fix", kCli, {L"--fix"}, kFix, true),
      Ok("FixAllOptions", kCli, {L"--fix", L"--take-back", L"--yes"}, kFix, true),
      Ok("ZoomShow", kCli, {L"--zoom"}, kZoom, false),
      Ok("ZoomSet", kCli, {L"--zoom", L"130"}, kZoom, true),
      Ok("ZoomSetEquals", kCli, {L"--zoom=130"}, kZoom, true),
      Ok("ZoomLowest", kCli, {L"--zoom", L"100"}, kZoom, true),
      Ok("ZoomHighest", kCli, {L"--zoom", L"200"}, kZoom, true),
      Ok("ZoomLeadingZero", kCli, {L"--zoom", L"0120"}, kZoom, true),
      Ok("ZoomNoRefresh", kCli, {L"--zoom", L"120", L"--no-refresh"}, kZoom, true),
      Ok("ChecksNone", kCli, {L"--checks", L"none"}, kChecks, true),
      Ok("ChecksBothEquals", kCli, {L"--checks=pdfcraft,signin"}, kChecks, true),
      Ok("ChecksPdfCraft", kCli, {L"--checks", L"pdfcraft"}, kChecks, true),
      Ok("ChecksSignIn", kCli, {L"--checks", L"signin"}, kChecks, true),
      Ok("ChecksDefault", kCli, {L"--checks", L"default"}, kChecks, true),
      Ok("Refresh", kCli, {L"--refresh"}, kRefresh, true),
      Ok("RefreshFull", kCli, {L"--refresh", L"--full", L"--yes"}, kRefresh, true),
      Ok("RefreshFolders", kCli, {L"--refresh", L"--folder", L"C:\\a", L"--folder=C:\\b"}, kRefresh,
         true),
      Ok("Install", kCli, {L"--install"}, kInstall, true),
      Ok("InstallMachine", kCli, {L"--install", L"--machine"}, kInstall, true),
      Ok("InstallUserAllOptions", kCli,
         {L"--install", L"--user", L"--checks", L"none", L"--zoom", L"150"}, kInstall, true),
      Ok("Uninstall", kCli, {L"--uninstall"}, kUninstall, true),
      Ok("UninstallAllOptions", kCli, {L"--uninstall", L"--yes", L"--rebuild-thumbnails"},
         kUninstall, true),
      Ok("Diagnostics", kCli, {L"--diagnostics"}, kDiagnostics, false),
      Ok("DiagnosticsCopy", kCli, {L"--diagnostics", L"--copy"}, kDiagnostics, false),
      Ok("DiagnosticsOut", kCli, {L"--diagnostics", L"--out", L"report.txt"}, kDiagnostics, false),
      Ok("MigrateV1", kCli, {L"--migrate-v1"}, kMigrateV1, true),
      Ok("MigrateV1UserOnly", kCli, {L"--migrate-v1", L"--user-only"}, kMigrateV1, true),
      Ok("MigrateV1MachineOnly", kCli, {L"--migrate-v1", L"--machine-only"}, kMigrateV1, true),
      Ok("SetupUser", kCli, {L"--setup-user", L"--checks=default", L"--zoom=130"}, kSetupUser,
         true),
      // Machine steps: the per-user canary does not guard them.
      Ok("SetupMachine", kGui, {L"--setup-machine"}, kSetupMachine, false),
      Ok("UninstallCleanup", kGui, {L"--uninstall-cleanup"}, kUninstallCleanup, false),
      Ok("ElevatedRepairClass", kGui, {L"--elevated", L"machine-repair-class"}, kElevated, false),
      Ok("ElevatedUninstallEquals", kGui, {L"--elevated=machine-uninstall"}, kElevated, false),
      Ok("ElevatedMigrate", kGui, {L"--elevated", L"machine-migrate-v1"}, kElevated, false),
      Ok("ElevatedInstallDev", kGui, {L"--elevated", L"machine-install"}, kElevated, false),

      // Unknown commands and options; case matters.
      Fails("Bogus", kCli, {L"--bogus"}, L"unknown command \"--bogus\""),
      Fails("CommandUppercase", kCli, {L"--Status"}, L"unknown command"),
      Fails("CommandAllCaps", kCli, {L"--STATUS"}, L"unknown command"),
      Fails("UnknownOption", kCli, {L"--status", L"--bogus"}, L"unknown option \"--bogus\""),
      Fails("OptionUppercase", kCli, {L"--status", L"--JSON"}, L"unknown option"),
      Fails("SingleDash", kCli, {L"-x"}, L"unknown command"),
      Fails("DoubleDashAlone", kCli, {L"--"}, L"unknown command"),
      Fails("Positional", kCli, {L"status"}, L"unexpected argument \"status\""),
      Fails("EmptyArgument", kCli, {L""}, L"unexpected argument"),
      Fails("ExtraArgument", kCli, {L"--status", L"extra"}, L"unexpected argument \"extra\""),
      // Misplaced: options of another command, a second command, options before the command.
      Fails("OptionOfAnotherCommand", kCli, {L"--status", L"--yes"},
            L"--status does not take --yes"),
      Fails("TwoCommands", kCli, {L"--status", L"--check"}, L"one command at a time"),
      Fails("CommandNamedLikeOption", kCli, {L"--status", L"--zoom", L"130"},
            L"one command at a time"),
      Fails("OptionFirst", kCli, {L"--json", L"--status"}, L"the command comes first"),
      Fails("RelaunchedBeforeCommand", kGui, {L"--relaunched", L"--status"},
            L"the command comes first"),
      Fails("OptionWithoutCommandGui", kGui, {L"--relaunched", L"--json"}, L"needs a command"),
      Fails("VersionTakesNoOptions", kCli, {L"--version", L"--json"}, L"does not take"),
      Fails("HelpThenCommand", kCli, {L"--help", L"--status"}, L"one command at a time"),
      Fails("HelpSpellingAsOption", kCli, {L"--status", L"-h"}, L"--help is a command of its own"),
      Fails("HelpSpellingWithArgument", kCli, {L"-h", L"x"}, L"unexpected argument"),
      // Values: missing, unexpected, empty, repeated.
      Fails("FlagWithValue", kCli, {L"--status", L"--json=1"}, L"--json takes no value"),
      Fails("CommandWithValue", kCli, {L"--status=1"}, L"--status takes no value"),
      Fails("ValueMissingAtEnd", kCli, {L"--status", L"--test-file"}, L"--test-file needs a value"),
      Fails("ValueIsAnOption", kCli, {L"--status", L"--test-file", L"--json"},
            L"--test-file needs a value"),
      Fails("ValueEmptyEquals", kCli, {L"--status", L"--test-file="}, L"--test-file needs a value"),
      Fails("ValueEmptySeparate", kCli, {L"--diagnostics", L"--out", L""}, L"--out needs a value"),
      Fails("FolderMissing", kCli, {L"--refresh", L"--folder"}, L"--folder needs a value"),
      Fails("GivenTwice", kCli, {L"--status", L"--json", L"--json"}, L"--json is given twice"),
      Fails("FlagGivenTwice", kCli, {L"--refresh", L"--full", L"--full"}, L"--full is given twice"),
      // Numbers: complete and in range.
      Fails("ZoomTooLow", kCli, {L"--zoom", L"99"}, L"from 100 to 200"),
      Fails("ZoomTooHigh", kCli, {L"--zoom", L"201"}, L"from 100 to 200"),
      Fails("ZoomNotANumber", kCli, {L"--zoom", L"abc"}, L"from 100 to 200"),
      Fails("ZoomTrailingText", kCli, {L"--zoom", L"12a"}, L"from 100 to 200"),
      Fails("ZoomDecimal", kCli, {L"--zoom", L"130.0"}, L"from 100 to 200"),
      Fails("ZoomSign", kCli, {L"--zoom", L"+130"}, L"from 100 to 200"),
      Fails("ZoomNegative", kCli, {L"--zoom", L"-130"}, L"from 100 to 200"),
      Fails("ZoomSpace", kCli, {L"--zoom", L" 130"}, L"from 100 to 200"),
      Fails("ZoomHuge", kCli, {L"--zoom", L"99999999999999999999999"}, L"from 100 to 200"),
      Fails("ZoomEmptyEquals", kCli, {L"--zoom="}, L"--zoom needs a value"),
      Fails("ZoomTwoValues", kCli, {L"--zoom", L"130", L"140"}, L"unexpected argument \"140\""),
      Fails("ZoomOptionOutOfRange", kCli, {L"--install", L"--zoom", L"250"}, L"from 100 to 200"),
      Fails("NoRefreshWithoutZoom", kCli, {L"--zoom", L"--no-refresh"}, L"--no-refresh goes with"),
      // --checks lists.
      Fails("ChecksMissing", kCli, {L"--checks"}, L"--checks needs a value"),
      Fails("ChecksUnknown", kCli, {L"--checks", L"both"}, L"--checks must be one of"),
      Fails("ChecksOtherOrder", kCli, {L"--checks", L"signin,pdfcraft"},
            L"--checks must be one of"),
      Fails("ChecksUppercase", kCli, {L"--checks", L"NONE"}, L"--checks must be one of"),
      Fails("ChecksOptionUnknown", kCli, {L"--setup-user", L"--checks", L"all"},
            L"--checks must be one of"),
      // Options that exclude each other.
      Fails("MachineAndUser", kCli, {L"--install", L"--machine", L"--user"},
            L"--machine and --user cannot be combined"),
      Fails("CopyAndOut", kCli, {L"--diagnostics", L"--out", L"x.txt", L"--copy"},
            L"--out and --copy cannot be combined"),
      Fails("UserOnlyAndMachineOnly", kCli, {L"--migrate-v1", L"--user-only", L"--machine-only"},
            L"cannot be combined"),
      Fails("FolderWithFull", kCli, {L"--refresh", L"--folder", L"C:\\a", L"--full"},
            L"--folder and --full cannot be combined"),
      Fails("FullWithFolder", kCli, {L"--refresh", L"--full", L"--folder=C:\\a"},
            L"--full and --folder cannot be combined"),
      Fails("YesWithoutFull", kCli, {L"--refresh", L"--yes"}, L"--yes goes with --full"),
      Fails("YesWithFolders", kCli, {L"--refresh", L"--folder", L"C:\\a", L"--yes"},
            L"--yes goes with --full"),
      // Developer-only and settings-executable-only parts.
      Fails("InstallInRelease", kCliRelease, {L"--install"}, L"unknown command"),
      Fails("ElevatedInCli", kCli, {L"--elevated", L"machine-uninstall"}, L"unknown command"),
      Fails("RelaunchedInCli", kCli, {L"--relaunched"}, L"unknown command"),
      Fails("RelaunchedOptionInCli", kCli, {L"--status", L"--relaunched"}, L"unknown option"),
      Fails("ElevatedNeedsStep", kGui, {L"--elevated"}, L"--elevated needs a value"),
      Fails("ElevatedNothingElse", kGui, {L"--elevated", L"machine-uninstall", L"--yes"},
            L"--elevated takes no other arguments"),
      Fails("ElevatedUnknownStep", kGui, {L"--elevated", L"format-disk"},
            L"unknown elevated step \"format-disk\""),
      Fails("ElevatedStepCase", kGui, {L"--elevated", L"Machine-Uninstall"},
            L"unknown elevated step"),
      Fails("ElevatedStepLooksLikeOption", kGui, {L"--elevated", L"-x"}, L"unknown elevated step"),
      Fails("ElevatedInstallInRelease", kGuiRelease, {L"--elevated", L"machine-install"},
            L"unknown elevated step"),
  };
  return cases;
}

class CliArgsTable : public ::testing::TestWithParam<size_t> {};

TEST_P(CliArgsTable, Parses) {
  const Case& test = Cases()[GetParam()];
  const auto result = ParseCommandLine(test.args, test.options);
  if (test.ok) {
    const CliArgs* args = std::get_if<CliArgs>(&result);
    ASSERT_NE(args, nullptr) << "error: "
                             << ::testing::PrintToString(std::get<ParseError>(result).message);
    EXPECT_EQ(args->verb, test.verb);
    EXPECT_EQ(args->writes, test.writes);
  } else {
    const ParseError* error = std::get_if<ParseError>(&result);
    ASSERT_NE(error, nullptr) << "parsed, but should have failed";
    EXPECT_NE(error->message.find(test.error), std::wstring::npos)
        << ::testing::PrintToString(error->message);
  }
}

INSTANTIATE_TEST_SUITE_P(Cases, CliArgsTable, ::testing::Range<size_t>(0, Cases().size()),
                         [](const ::testing::TestParamInfo<size_t>& info) {
                           return std::string(Cases()[info.param].name);
                         });

TEST(CliArgs, TableIsLarge) { EXPECT_GE(Cases().size(), 60u); }

CliArgs ParseOk(const ParseOptions& options, std::initializer_list<std::wstring_view> words) {
  auto result = Parse(options, words);
  if (const ParseError* error = std::get_if<ParseError>(&result)) {
    ADD_FAILURE() << ::testing::PrintToString(error->message);
    return {};
  }
  return std::get<CliArgs>(std::move(result));
}

TEST(CliArgs, StatusValues) {
  const CliArgs args =
      ParseOk(kCli, {L"--status", L"--json", L"--no-live-test", L"--test-file=C:\\a b\\x.pdf"});
  EXPECT_TRUE(args.json);
  EXPECT_TRUE(args.noLiveTest);
  ASSERT_TRUE(args.testFile);
  EXPECT_EQ(*args.testFile, L"C:\\a b\\x.pdf");
}

TEST(CliArgs, CheckAndFixValues) {
  const CliArgs check = ParseOk(kCli, {L"--check", L"--quiet", L"--no-repair"});
  EXPECT_TRUE(check.quiet);
  EXPECT_TRUE(check.noRepair);
  EXPECT_FALSE(check.noLiveTest);
  const CliArgs fix = ParseOk(kCli, {L"--fix", L"--take-back", L"--yes"});
  EXPECT_TRUE(fix.takeBack);
  EXPECT_TRUE(fix.yes);
}

TEST(CliArgs, ZoomValues) {
  EXPECT_FALSE(ParseOk(kCli, {L"--zoom"}).zoom);
  const CliArgs set = ParseOk(kCli, {L"--zoom", L"0130", L"--no-refresh"});
  ASSERT_TRUE(set.zoom);
  EXPECT_EQ(*set.zoom, 130);
  EXPECT_TRUE(set.noRefresh);
}

TEST(CliArgs, ChecksValues) {
  EXPECT_EQ(ParseOk(kCli, {L"--checks", L"pdfcraft,signin"}).checks, (ChecksSelection{true, true}));
  EXPECT_EQ(ParseOk(kCli, {L"--checks", L"default"}).checks, (ChecksSelection{true, true}));
  EXPECT_EQ(ParseOk(kCli, {L"--checks", L"pdfcraft"}).checks, (ChecksSelection{true, false}));
  EXPECT_EQ(ParseOk(kCli, {L"--checks", L"signin"}).checks, (ChecksSelection{false, true}));
  EXPECT_EQ(ParseOk(kCli, {L"--checks", L"none"}).checks, (ChecksSelection{false, false}));
}

TEST(CliArgs, RefreshFoldersRepeat) {
  const CliArgs args = ParseOk(kCli, {L"--refresh", L"--folder", L"C:\\a", L"--folder=C:\\b c"});
  EXPECT_EQ(args.folders, (std::vector<std::wstring>{L"C:\\a", L"C:\\b c"}));
  EXPECT_FALSE(args.full);
  EXPECT_FALSE(args.yes);
  const CliArgs full = ParseOk(kCli, {L"--refresh", L"--full", L"--yes"});
  EXPECT_TRUE(full.folders.empty());
  EXPECT_TRUE(full.full);
  EXPECT_TRUE(full.yes);
}

TEST(CliArgs, InstallValues) {
  EXPECT_EQ(ParseOk(kCli, {L"--install"}).scope, Scope::kMachine);
  const CliArgs args =
      ParseOk(kCli, {L"--install", L"--user", L"--checks", L"signin", L"--zoom=200"});
  EXPECT_EQ(args.scope, Scope::kUser);
  EXPECT_EQ(args.checks, (ChecksSelection{false, true}));
  EXPECT_EQ(args.zoom, 200);
  EXPECT_FALSE(args.force);
  EXPECT_TRUE(ParseOk(kCli, {L"--install", L"--user", L"--force"}).force);
  // --force belongs to --install alone.
  EXPECT_TRUE(std::holds_alternative<ParseError>(Parse(kCli, {L"--uninstall", L"--force"})));
  EXPECT_TRUE(std::holds_alternative<ParseError>(Parse(kCli, {L"--install", L"--force=1"})));
  EXPECT_TRUE(std::holds_alternative<ParseError>(Parse(kCliRelease, {L"--install", L"--force"})));
}

TEST(CliArgs, UninstallDiagnosticsMigrateValues) {
  const CliArgs uninstall = ParseOk(kCli, {L"--uninstall", L"--rebuild-thumbnails"});
  EXPECT_TRUE(uninstall.rebuildThumbnails);
  EXPECT_FALSE(uninstall.yes);
  EXPECT_TRUE(ParseOk(kCli, {L"--diagnostics", L"--copy"}).copy);
  EXPECT_EQ(ParseOk(kCli, {L"--diagnostics", L"--out", L"r.txt"}).out, L"r.txt");
  EXPECT_EQ(ParseOk(kCli, {L"--migrate-v1"}).migrateScope, MigrateScope::kBoth);
  EXPECT_EQ(ParseOk(kCli, {L"--migrate-v1", L"--user-only"}).migrateScope, MigrateScope::kUserOnly);
  EXPECT_EQ(ParseOk(kCli, {L"--migrate-v1", L"--machine-only"}).migrateScope,
            MigrateScope::kMachineOnly);
}

TEST(CliArgs, ElevatedSteps) {
  EXPECT_EQ(ParseOk(kGui, {L"--elevated", L"machine-install"}).elevatedStep,
            ElevatedStep::kMachineInstall);
  EXPECT_EQ(ParseOk(kGui, {L"--elevated", L"machine-repair-class"}).elevatedStep,
            ElevatedStep::kMachineRepairClass);
  EXPECT_EQ(ParseOk(kGui, {L"--elevated", L"machine-uninstall"}).elevatedStep,
            ElevatedStep::kMachineUninstall);
  EXPECT_EQ(ParseOk(kGui, {L"--elevated", L"machine-migrate-v1"}).elevatedStep,
            ElevatedStep::kMachineMigrateV1);
}

TEST(CliArgs, Relaunched) {
  EXPECT_TRUE(ParseOk(kGui, {L"--relaunched"}).relaunched);
  EXPECT_TRUE(ParseOk(kGui, {L"--check", L"--relaunched"}).relaunched);
  EXPECT_FALSE(ParseOk(kGui, {}).relaunched);
}

TEST(CliArgs, WritesColumn) {
  struct Row {
    Verb verb;
    Writes writes;
  };
  const Row expected[] = {
      {Verb::kNone, Writes::kNo},
      {Verb::kStatus, Writes::kNo},
      {Verb::kCheck, Writes::kYes},
      {Verb::kFix, Writes::kYes},
      {Verb::kZoom, Writes::kWithValue},
      {Verb::kChecks, Writes::kYes},
      {Verb::kRefresh, Writes::kYes},
      {Verb::kInstall, Writes::kYes},
      {Verb::kUninstall, Writes::kYes},
      {Verb::kDiagnostics, Writes::kNo},
      {Verb::kMigrateV1, Writes::kYes},
      {Verb::kSetupUser, Writes::kYes},
      // Elevated installer steps: never refused by the per-user canary.
      {Verb::kSetupMachine, Writes::kMachine},
      {Verb::kUninstallCleanup, Writes::kMachine},
      {Verb::kVersion, Writes::kNo},
      {Verb::kHelp, Writes::kNo},
      {Verb::kElevated, Writes::kNo},
  };
  ASSERT_EQ(std::size(expected), Verbs().size());
  for (const Row& row : expected) {
    EXPECT_EQ(Describe(row.verb).writes, row.writes) << static_cast<int>(row.verb);
    EXPECT_EQ(Describe(row.verb).verb, row.verb);
  }
  // Only the report commands compare registry views.
  for (const VerbInfo& verb : Verbs()) {
    EXPECT_EQ(verb.reportsState, verb.verb == Verb::kStatus || verb.verb == Verb::kDiagnostics)
        << static_cast<int>(verb.verb);
  }
}

TEST(CliArgs, ZoomValueSyntax) {
  EXPECT_EQ(ParseZoomValue(L"100"), 100);
  EXPECT_EQ(ParseZoomValue(L"200"), 200);
  EXPECT_EQ(ParseZoomValue(L"141"), 141);
  EXPECT_FALSE(ParseZoomValue(L""));
  EXPECT_FALSE(ParseZoomValue(L"99"));
  EXPECT_FALSE(ParseZoomValue(L"201"));
  EXPECT_FALSE(ParseZoomValue(L"1e2"));
  EXPECT_FALSE(ParseZoomValue(L"\xFF11\xFF13\xFF10"));  // full-width digits
  EXPECT_FALSE(ParseZoomValue(L"4294967426"));          // 130 after a 32-bit wrap
}

TEST(CliArgs, HelpListsEveryVisibleCommandAndExitCode) {
  for (const ParseOptions& options : {kCli, kGui}) {
    const std::wstring text = UsageText(options);
    for (const VerbInfo& verb : Verbs()) {
      if (verb.verb == Verb::kNone) continue;
      const bool listed = text.find(std::wstring(verb.name) + L" ") != std::wstring::npos ||
                          text.find(std::wstring(verb.name) + L"\n") != std::wstring::npos;
      EXPECT_EQ(listed, !verb.hidden) << ::testing::PrintToString(std::wstring(verb.name));
    }
    EXPECT_EQ(text.find(L"--relaunched"), std::wstring::npos);
    EXPECT_EQ(text.find(L"{"), std::wstring::npos) << "a placeholder was not filled in";
    EXPECT_NE(text.find(kPdfCraftProductName), std::wstring::npos);
    for (const ExitCodeInfo& row : kExitCodes) {
      EXPECT_NE(
          text.find(L"  " + std::to_wstring(static_cast<int>(row.code)) + L"  " + row.meaning),
          std::wstring::npos)
          << static_cast<int>(row.code);
    }
    EXPECT_NE(text.find(options.mode == Mode::kCli ? kCliExe : kSettingsExe), std::wstring::npos);
    EXPECT_NE(text.back(), L'\n');
  }
  EXPECT_NE(UsageText(kCli).find(L"[--machine|--user]"), std::wstring::npos);
  EXPECT_NE(UsageText(kCli).find(L"--refresh [--folder <folder>]... [--full] [--yes]\n"),
            std::wstring::npos);
  EXPECT_NE(UsageText(kCli).find(L"--zoom [<100-200>]"), std::wstring::npos);
  EXPECT_EQ(UsageText(kCliRelease).find(L"--install"), std::wstring::npos);
}

// The rebuild restarts File Explorer: the help says that it asks first, also for the uninstall's
// rebuild. Exit code 3 covers a Ctrl+C before anything changed, and one that stopped
// --refresh --folder part way; exit code 8 another account's File Explorer for either refresh.
TEST(CliArgs, HelpSaysTheRebuildAsksFirst) {
  const std::wstring text = UsageText(kCli);
  EXPECT_NE(text.find(L"which restarts File Explorer (asks first, or --yes)."), std::wstring::npos);
  EXPECT_NE(text.find(L"for all users of this PC (asks first, or --yes). --rebuild-thumbnails also "
                      L"rebuilds the thumbnail cache, which restarts File Explorer.\n"),
            std::wstring::npos);
  EXPECT_NE(text.find(L"  3  cancelled (the administrator prompt was declined, the answer was no, "
                      L"or Ctrl+C stopped it before anything changed; --refresh --folder keeps the "
                      L"thumbnails it refreshed until then)\n"),
            std::wstring::npos);
  EXPECT_NE(text.find(L"elevated where that is refused, a refresh of thumbnails where File "
                      L"Explorer belongs to another account, or a rebuild of the thumbnail cache "
                      L"where File Explorer would not come back from here)"),
            std::wstring::npos);
}

}  // namespace
}  // namespace pdft
