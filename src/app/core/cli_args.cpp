#include "cli_args.h"

#include <algorithm>
#include <array>

#include "project_config.h"
#include "zoom.h"

namespace pdft {
namespace {

using VerbMask = std::uint32_t;

constexpr VerbMask Bit(Verb verb) { return VerbMask{1} << static_cast<int>(verb); }

// The commands that do real work, as opposed to --version, --help and --elevated.
constexpr VerbMask kWorkingVerbs =
    Bit(Verb::kStatus) | Bit(Verb::kCheck) | Bit(Verb::kFix) | Bit(Verb::kZoom) |
    Bit(Verb::kChecks) | Bit(Verb::kRefresh) | Bit(Verb::kInstall) | Bit(Verb::kUninstall) |
    Bit(Verb::kDiagnostics) | Bit(Verb::kMigrateV1) | Bit(Verb::kSetupUser) |
    Bit(Verb::kSetupMachine) | Bit(Verb::kUninstallCleanup);

using enum ValueKind;

// The command table. Columns: verb, name, value, value name, writes, reports state, settings
// executable only, developer builds only, hidden, summary.
constexpr VerbInfo kVerbs[] = {
    {Verb::kNone, L"", kNone, L"", Writes::kNo, false, false, false, true, L""},
    {Verb::kStatus, L"--status", kNone, L"", Writes::kNo, true, false, false, false,
     L"Report whether PDF thumbnails work, and why not. Changes nothing."},
    {Verb::kCheck, L"--check", kNone, L"", Writes::kYes, false, false, false, false,
     L"Check, and repair what is safe to repair (what the scheduled checks run)."},
    {Verb::kFix, L"--fix", kNone, L"", Writes::kYes, false, false, false, false,
     L"Repair what --check repairs; --take-back also takes the .pdf slots back from another "
     L"handler (asks first, or --yes). The part for all users needs administrator rights."},
    {Verb::kZoom, L"--zoom", kOptional, L"<100-200>", Writes::kWithValue, false, false, false,
     false,
     L"Show the thumbnail zoom and where it comes from, or set it (100 shows the whole page)."},
    {Verb::kChecks, L"--checks", kRequired, L"<list>", Writes::kYes, false, false, false, false,
     L"Choose when the scheduled checks run: after {pdfcraft} updates, at sign-in, both or never."},
    {Verb::kRefresh, L"--refresh", kNone, L"", Writes::kYes, false, false, false, false,
     L"Refresh the PDF thumbnails File Explorer already made: in --folder folders and their "
     L"subfolders, or in open folder windows, on the desktop and in the PDFs Windows Search "
     L"knows. --full rebuilds the whole thumbnail cache instead, which restarts File Explorer "
     L"(asks first, or --yes)."},
    {Verb::kInstall, L"--install", kNone, L"", Writes::kYes, false, false, true, false,
     L"Install {product} from this folder (developer builds)."},
    {Verb::kUninstall, L"--uninstall", kNone, L"", Writes::kYes, false, false, false, false,
     L"Remove {product} for all users of this PC (asks first, or --yes). --rebuild-thumbnails "
     L"also rebuilds the thumbnail cache, which restarts File Explorer."},
    {Verb::kDiagnostics, L"--diagnostics", kNone, L"", Writes::kNo, true, false, false, false,
     L"Print a report for bug reports, with personal details masked."},
    {Verb::kMigrateV1, L"--migrate-v1", kNone, L"", Writes::kYes, false, false, false, false,
     L"Remove what version 1.x left behind, keeping its zoom."},
    {Verb::kSetupUser, L"--setup-user", kNone, L"", Writes::kYes, false, false, false, false,
     L"Installer step: set up the signed-in user."},
    {Verb::kSetupMachine, L"--setup-machine", kNone, L"", Writes::kMachine, false, false, false,
     false, L"Installer step: set up this PC (runs as administrator)."},
    {Verb::kUninstallCleanup, L"--uninstall-cleanup", kNone, L"", Writes::kMachine, false, false,
     false, false, L"Uninstaller step: remove what the uninstaller leaves behind."},
    {Verb::kVersion, L"--version", kNone, L"", Writes::kNo, false, false, false, false,
     L"Print the version."},
    {Verb::kHelp, L"--help", kNone, L"", Writes::kNo, false, false, false, false,
     L"Print this help (also -h, -? and /?)."},
    {Verb::kElevated, L"--elevated", kRequired, L"<step>", Writes::kNo, false, true, false, true,
     L"Internal: the elevated half of a change."},
};

consteval bool VerbsInEnumOrder() {
  for (size_t i = 0; i < std::size(kVerbs); ++i) {
    if (static_cast<size_t>(kVerbs[i].verb) != i) return false;
  }
  return std::size(kVerbs) == static_cast<size_t>(Verb::kElevated) + 1;
}
static_assert(VerbsInEnumOrder(), "kVerbs must list every Verb in enum order");

enum class Option {
  kJson,
  kNoLiveTest,
  kTestFile,
  kQuiet,
  kNoRepair,
  kTakeBack,
  kNoRefresh,
  kFolder,
  kFull,
  kMachine,
  kUser,
  kChecks,
  kZoom,
  kForce,
  kRebuildThumbnails,
  kYes,
  kCopy,
  kOut,
  kUserOnly,
  kMachineOnly,
  kRelaunched,
};

struct OptionInfo {
  Option option;
  std::wstring_view name;
  ValueKind value;  // kNone or kRequired
  std::wstring_view valueName;
  bool repeatable;
  int group;  // options with the same group (not 0) exclude each other
  bool guiOnly;
  bool hidden;
  VerbMask verbs;  // the commands that take it
};

// The option table, in help order. Columns: option, name, value, value name, repeatable,
// exclusive group, settings executable only, hidden, commands.
constexpr OptionInfo kOptions[] = {
    {Option::kJson, L"--json", kNone, L"", false, 0, false, false, Bit(Verb::kStatus)},
    {Option::kNoLiveTest, L"--no-live-test", kNone, L"", false, 0, false, false,
     Bit(Verb::kStatus) | Bit(Verb::kCheck)},
    {Option::kTestFile, L"--test-file", kRequired, L"<pdf>", false, 0, false, false,
     Bit(Verb::kStatus)},
    {Option::kQuiet, L"--quiet", kNone, L"", false, 0, false, false, Bit(Verb::kCheck)},
    {Option::kNoRepair, L"--no-repair", kNone, L"", false, 0, false, false, Bit(Verb::kCheck)},
    {Option::kTakeBack, L"--take-back", kNone, L"", false, 0, false, false, Bit(Verb::kFix)},
    {Option::kNoRefresh, L"--no-refresh", kNone, L"", false, 0, false, false, Bit(Verb::kZoom)},
    {Option::kFolder, L"--folder", kRequired, L"<folder>", true, 4, false, false,
     Bit(Verb::kRefresh)},
    {Option::kFull, L"--full", kNone, L"", false, 4, false, false, Bit(Verb::kRefresh)},
    {Option::kMachine, L"--machine", kNone, L"", false, 1, false, false, Bit(Verb::kInstall)},
    {Option::kUser, L"--user", kNone, L"", false, 1, false, false, Bit(Verb::kInstall)},
    {Option::kChecks, L"--checks", kRequired, L"<list>", false, 0, false, false,
     Bit(Verb::kInstall) | Bit(Verb::kSetupUser)},
    {Option::kZoom, L"--zoom", kRequired, L"<100-200>", false, 0, false, false,
     Bit(Verb::kInstall) | Bit(Verb::kSetupUser)},
    {Option::kForce, L"--force", kNone, L"", false, 0, false, false, Bit(Verb::kInstall)},
    {Option::kRebuildThumbnails, L"--rebuild-thumbnails", kNone, L"", false, 0, false, false,
     Bit(Verb::kUninstall)},
    {Option::kYes, L"--yes", kNone, L"", false, 0, false, false,
     Bit(Verb::kFix) | Bit(Verb::kRefresh) | Bit(Verb::kUninstall)},
    {Option::kCopy, L"--copy", kNone, L"", false, 2, false, false, Bit(Verb::kDiagnostics)},
    {Option::kOut, L"--out", kRequired, L"<file>", false, 2, false, false, Bit(Verb::kDiagnostics)},
    {Option::kUserOnly, L"--user-only", kNone, L"", false, 3, false, false, Bit(Verb::kMigrateV1)},
    {Option::kMachineOnly, L"--machine-only", kNone, L"", false, 3, false, false,
     Bit(Verb::kMigrateV1)},
    {Option::kRelaunched, L"--relaunched", kNone, L"", false, 0, true, true,
     Bit(Verb::kNone) | kWorkingVerbs},
};

struct ElevatedStepInfo {
  ElevatedStep step;
  std::wstring_view name;
  bool devOnly;
};

constexpr ElevatedStepInfo kElevatedSteps[] = {
    {ElevatedStep::kMachineInstall, L"machine-install", true},
    {ElevatedStep::kMachineRepairClass, L"machine-repair-class", false},
    {ElevatedStep::kMachineUninstall, L"machine-uninstall", false},
    {ElevatedStep::kMachineMigrateV1, L"machine-migrate-v1", false},
};

constexpr std::wstring_view kChecksValues = L"pdfcraft,signin | pdfcraft | signin | none | default";

// A token split at its first '=': "--zoom=130" is {"--zoom", "130", true}.
struct Token {
  std::wstring_view name;
  std::wstring_view value;
  bool hasValue = false;
};

Token Split(std::wstring_view token) {
  const size_t equals = token.find(L'=');
  if (equals == std::wstring_view::npos || !token.starts_with(L"--")) return {token, {}, false};
  return {token.substr(0, equals), token.substr(equals + 1), true};
}

bool IsHelpSpelling(std::wstring_view token) {
  return token == L"-h" || token == L"-?" || token == L"/?";
}

bool Available(const VerbInfo& verb, const ParseOptions& options) {
  return (!verb.guiOnly || options.mode == Mode::kGui) && (!verb.devOnly || options.devInstall);
}

const VerbInfo* FindVerb(std::wstring_view name, const ParseOptions& options) {
  for (const VerbInfo& verb : kVerbs) {
    if (verb.verb != Verb::kNone && verb.name == name && Available(verb, options)) return &verb;
  }
  return nullptr;
}

const OptionInfo* FindOption(std::wstring_view name, const ParseOptions& options) {
  for (const OptionInfo& option : kOptions) {
    if (option.name == name && (!option.guiOnly || options.mode == Mode::kGui)) return &option;
  }
  return nullptr;
}

std::optional<ElevatedStep> ParseElevatedStep(std::wstring_view text, const ParseOptions& options) {
  for (const ElevatedStepInfo& step : kElevatedSteps) {
    if (step.name == text && (!step.devOnly || options.devInstall)) return step.step;
  }
  return std::nullopt;
}

std::wstring Quoted(std::wstring_view text) { return L"\"" + std::wstring(text) + L"\""; }

ParseError Error(std::wstring message) { return ParseError{std::move(message)}; }

// Stores the value of the command itself ("--zoom 130"); an error message if it is invalid.
std::optional<std::wstring> ApplyVerbValue(const VerbInfo& verb, std::wstring_view value,
                                           const ParseOptions& options, CliArgs* args) {
  switch (verb.verb) {
    case Verb::kZoom:
      args->zoom = ParseZoomValue(value);
      if (!args->zoom) return L"--zoom must be a whole number from 100 to 200";
      return std::nullopt;
    case Verb::kChecks:
      args->checks = ParseChecksValue(value);
      if (!args->checks) return L"--checks must be one of " + std::wstring(kChecksValues);
      return std::nullopt;
    case Verb::kElevated:
      args->elevatedStep = ParseElevatedStep(value, options);
      if (!args->elevatedStep) return L"unknown elevated step " + Quoted(value);
      return std::nullopt;
    default:
      return std::wstring(verb.name) + L" takes no value";
  }
}

// Stores one option; an error message if its value is invalid.
std::optional<std::wstring> ApplyOption(Option option, std::wstring_view value, CliArgs* args) {
  switch (option) {
    case Option::kJson:
      args->json = true;
      break;
    case Option::kNoLiveTest:
      args->noLiveTest = true;
      break;
    case Option::kTestFile:
      args->testFile = std::wstring(value);
      break;
    case Option::kQuiet:
      args->quiet = true;
      break;
    case Option::kNoRepair:
      args->noRepair = true;
      break;
    case Option::kTakeBack:
      args->takeBack = true;
      break;
    case Option::kNoRefresh:
      args->noRefresh = true;
      break;
    case Option::kFolder:
      args->folders.emplace_back(value);
      break;
    case Option::kFull:
      args->full = true;
      break;
    case Option::kMachine:
      args->scope = Scope::kMachine;
      break;
    case Option::kUser:
      args->scope = Scope::kUser;
      break;
    case Option::kChecks:
      args->checks = ParseChecksValue(value);
      if (!args->checks) return L"--checks must be one of " + std::wstring(kChecksValues);
      break;
    case Option::kZoom:
      args->zoom = ParseZoomValue(value);
      if (!args->zoom) return L"--zoom must be a whole number from 100 to 200";
      break;
    case Option::kForce:
      args->force = true;
      break;
    case Option::kRebuildThumbnails:
      args->rebuildThumbnails = true;
      break;
    case Option::kYes:
      args->yes = true;
      break;
    case Option::kCopy:
      args->copy = true;
      break;
    case Option::kOut:
      args->out = std::wstring(value);
      break;
    case Option::kUserOnly:
      args->migrateScope = MigrateScope::kUserOnly;
      break;
    case Option::kMachineOnly:
      args->migrateScope = MigrateScope::kMachineOnly;
      break;
    case Option::kRelaunched:
      args->relaunched = true;
      break;
  }
  return std::nullopt;
}

// A separate value: the next argument, unless there is none or it is another option.
bool TakesNext(std::span<const std::wstring_view> args, size_t index) {
  return index + 1 < args.size() && !args[index + 1].starts_with(L"--");
}

// The summary with its placeholders filled in.
std::wstring Fill(std::wstring_view text) {
  struct Placeholder {
    std::wstring_view key;
    std::wstring_view value;
  };
  const Placeholder placeholders[] = {{L"{product}", kProductName},
                                      {L"{pdfcraft}", kPdfCraftProductName},
                                      {L"{gui}", kSettingsExe}};
  std::wstring result(text);
  for (const Placeholder& placeholder : placeholders) {
    for (size_t at = result.find(placeholder.key); at != std::wstring::npos;
         at = result.find(placeholder.key, at + placeholder.value.size())) {
      result.replace(at, placeholder.key.size(), placeholder.value);
    }
  }
  return result;
}

// "--install [--machine|--user] [--checks <list>] [--zoom <100-200>]"
std::wstring Syntax(const VerbInfo& verb, const ParseOptions& options) {
  std::wstring text(verb.name);
  if (verb.value == kOptional) text += L" [" + std::wstring(verb.valueName) + L"]";
  if (verb.value == kRequired) text += L" " + std::wstring(verb.valueName);
  int openGroup = 0;
  for (const OptionInfo& option : kOptions) {
    if (!(option.verbs & Bit(verb.verb)) || option.hidden ||
        (option.guiOnly && options.mode != Mode::kGui)) {
      continue;
    }
    if (option.group != 0 && option.group == openGroup) {
      text.back() = L'|';  // "[--machine]" becomes "[--machine|--user]"
    } else {
      text += L" [";
    }
    text += option.name;
    if (option.value == kRequired) text += L" " + std::wstring(option.valueName);
    text += L"]";
    if (option.repeatable) text += L"...";
    // "[--folder <folder>]..." stays whole: the next option of its group gets its own brackets,
    // so the syntax does not show that they exclude each other and the summary has to say it.
    openGroup = option.repeatable ? 0 : option.group;
  }
  return text;
}

}  // namespace

std::span<const VerbInfo> Verbs() noexcept { return kVerbs; }

const VerbInfo& Describe(Verb verb) noexcept { return kVerbs[static_cast<size_t>(verb)]; }

std::optional<int> ParseZoomValue(std::wstring_view text) noexcept {
  if (text.empty()) return std::nullopt;
  long long value = 0;
  for (wchar_t c : text) {
    if (c < L'0' || c > L'9') return std::nullopt;
    value = std::min(value * 10 + (c - L'0'), 1'000'000LL);  // stays out of range, never wraps
  }
  if (value < kMinZoom || value > kMaxZoom) return std::nullopt;
  return static_cast<int>(value);
}

std::optional<ChecksSelection> ParseChecksValue(std::wstring_view text) noexcept {
  if (text == L"pdfcraft,signin" || text == L"default") return ChecksSelection{true, true};
  if (text == L"pdfcraft") return ChecksSelection{true, false};
  if (text == L"signin") return ChecksSelection{false, true};
  if (text == L"none") return ChecksSelection{false, false};
  return std::nullopt;
}

std::variant<CliArgs, ParseError> ParseCommandLine(std::span<const std::wstring_view> args,
                                                   const ParseOptions& options) {
  CliArgs result;
  if (args.empty()) return result;  // no command

  // The command.
  const VerbInfo* verb = nullptr;
  size_t index = 0;
  bool verbValue = false;  // the command's own value was given
  const Token first = Split(args[0]);
  if (IsHelpSpelling(args[0])) {
    verb = &Describe(Verb::kHelp);
    index = 1;
  } else if ((verb = FindVerb(first.name, options)) != nullptr) {
    index = 1;
    verbValue = first.hasValue;
    std::wstring_view value = first.value;
    if (verb->value == kNone) {
      if (verbValue) return Error(std::wstring(verb->name) + L" takes no value");
    } else {
      if (!verbValue && TakesNext(args, 0)) {
        value = args[1];
        verbValue = true;
        index = 2;
      }
      if ((verbValue && value.empty()) || (!verbValue && verb->value == kRequired)) {
        return Error(std::wstring(verb->name) + L" needs a value");
      }
      if (verbValue) {
        if (auto error = ApplyVerbValue(*verb, value, options, &result)) return Error(*error);
      }
    }
    if (verb->verb == Verb::kElevated && index < args.size()) {
      return Error(L"--elevated takes no other arguments");
    }
  } else if (const OptionInfo* option = FindOption(first.name, options);
             option && (option->verbs & Bit(Verb::kNone))) {
    verb = &Describe(Verb::kNone);  // options only (--relaunched): no command
  } else if (option) {
    return Error(L"the command comes first; " + std::wstring(first.name) + L" is an option");
  } else if (args[0].starts_with(L"-")) {
    return Error(L"unknown command " + Quoted(args[0]));
  } else {
    return Error(L"unexpected argument " + Quoted(args[0]));
  }
  result.verb = verb->verb;

  // Its options.
  std::array<bool, std::size(kOptions)> seen = {};
  for (; index < args.size(); ++index) {
    const std::wstring_view word = args[index];
    if (IsHelpSpelling(word)) return Error(L"--help is a command of its own");
    if (!word.starts_with(L"-")) return Error(L"unexpected argument " + Quoted(word));
    const Token token = Split(word);
    const OptionInfo* option = FindOption(token.name, options);
    if (!option || !(option->verbs & Bit(verb->verb))) {
      if (FindVerb(token.name, options)) {
        if (verb->verb == Verb::kNone) {
          return Error(L"the command comes first: " + std::wstring(token.name));
        }
        return Error(L"one command at a time: " + std::wstring(verb->name) + L" and " +
                     std::wstring(token.name));
      }
      if (option && verb->verb != Verb::kNone) {
        return Error(std::wstring(verb->name) + L" does not take " + std::wstring(token.name));
      }
      if (option) return Error(std::wstring(token.name) + L" needs a command");
      return Error(L"unknown option " + Quoted(token.name));
    }
    const size_t which = static_cast<size_t>(option - kOptions);
    if (seen[which] && !option->repeatable) {
      return Error(std::wstring(option->name) + L" is given twice");
    }
    for (size_t other = 0; other < std::size(kOptions); ++other) {
      if (seen[other] && other != which && option->group != 0 &&
          kOptions[other].group == option->group) {
        return Error(std::wstring(kOptions[other].name) + L" and " + std::wstring(option->name) +
                     L" cannot be combined");
      }
    }
    std::wstring_view value;
    if (option->value == kNone) {
      if (token.hasValue) return Error(std::wstring(option->name) + L" takes no value");
    } else if (token.hasValue) {
      value = token.value;
    } else if (TakesNext(args, index)) {
      value = args[++index];
    } else {
      return Error(std::wstring(option->name) + L" needs a value");
    }
    if (option->value == kRequired && value.empty()) {
      return Error(std::wstring(option->name) + L" needs a value");
    }
    if (auto error = ApplyOption(option->option, value, &result)) return Error(*error);
    seen[which] = true;
  }

  if (result.noRefresh && !result.zoom) {
    return Error(L"--no-refresh goes with a new zoom: --zoom <100-200> --no-refresh");
  }
  if (verb->verb == Verb::kRefresh && result.yes && !result.full) {
    return Error(L"--yes goes with --full: --refresh --full --yes");
  }
  result.writes = verb->writes == Writes::kYes || (verb->writes == Writes::kWithValue && verbValue);
  return result;
}

std::wstring UsageText(const ParseOptions& options) {
  const std::wstring exe = options.mode == Mode::kCli ? kCliExe : kSettingsExe;
  std::wstring text = std::wstring(kProductName) + L" " + kProductVersion + L" (" + kArch + L"): " +
                      kDescription + L"\n\nUsage:\n  " + exe + L" <command> [options]\n  " + exe +
                      L"   (no command) ";
  text += options.mode == Mode::kCli
              ? L"starts " + std::wstring(kSettingsExe) + L", the settings window"
              : std::wstring(L"opens the settings window");
  text += L"\n\nCommands:\n";
  for (const VerbInfo& verb : kVerbs) {
    if (verb.hidden || !Available(verb, options)) continue;
    text += L"  " + Syntax(verb, options) + L"\n      " + Fill(verb.summary) + L"\n";
  }
  text += L"\nValues go as --name value or --name=value.\n<list> is one of: ";
  text += kChecksValues;
  text += L" (default = pdfcraft,signin).\n\nExit codes:\n";
  for (const ExitCodeInfo& row : kExitCodes) {
    text += L"  " + std::to_wstring(static_cast<int>(row.code)) + L"  " + row.meaning + L"\n";
  }
  text.pop_back();  // the caller ends the last line
  return text;
}

}  // namespace pdft
