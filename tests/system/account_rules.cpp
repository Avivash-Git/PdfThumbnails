#include "account_rules.h"

#include <cwchar>
#include <utility>

namespace pdft::systest {
namespace {

constexpr std::wstring_view kUpper = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::wstring_view kLower = L"abcdefghijklmnopqrstuvwxyz";
constexpr std::wstring_view kDigits = L"0123456789";
// No quotes, backslashes or spaces: nothing that could ever need escaping.
constexpr std::wstring_view kSymbols = L"!#%*+-.:=?@^_~";

constexpr std::wstring_view kNamePrefix = L"pdft-sys-";
constexpr size_t kNameHexDigits = 6;

wchar_t AsciiLower(wchar_t c) noexcept {
  return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// Whether |text| holds |part|, ignoring the case of ASCII letters; nothing is copied.
bool HoldsIgnoringCase(std::wstring_view text, std::wstring_view part) noexcept {
  if (part.empty() || part.size() > text.size()) return false;
  for (size_t start = 0; start + part.size() <= text.size(); ++start) {
    size_t i = 0;
    while (i < part.size() && AsciiLower(text[start + i]) == AsciiLower(part[i])) ++i;
    if (i == part.size()) return true;
  }
  return false;
}

// Whether |password| holds |accountName|, or a part of it of three or more characters between its
// dashes, ignoring case.
bool HoldsTheName(std::wstring_view password, std::wstring_view accountName) noexcept {
  if (accountName.size() >= 3 && HoldsIgnoringCase(password, accountName)) return true;
  size_t start = 0;
  while (start < accountName.size()) {
    size_t end = accountName.find(L'-', start);
    if (end == std::wstring_view::npos) end = accountName.size();
    const std::wstring_view part = accountName.substr(start, end - start);
    if (part.size() >= 3 && HoldsIgnoringCase(password, part)) return true;
    start = end + 1;
  }
  return false;
}

// The test name after |tag| at the start of |line| (up to the first space: an end line goes on
// with its time); std::nullopt when the line does not start with |tag|.
std::optional<std::string_view> NameAfter(std::string_view line, std::string_view tag) {
  if (!line.starts_with(tag)) return std::nullopt;
  line.remove_prefix(tag.size());
  return line.substr(0, line.find(' '));
}

std::string ResultText(const GTestCase& test) {
  switch (test.result) {
    case GTestCase::Result::kPassed:
      return "passed";
    case GTestCase::Result::kFailed:
      return "failed";
    case GTestCase::Result::kSkipped:
      return "skipped (" + test.skipReason + ")";
    case GTestCase::Result::kUnfinished:
      break;
  }
  return "did not finish";
}

bool StartsWithOneOf(std::string_view text, std::span<const char* const> starts) {
  for (const char* start : starts) {
    if (text.starts_with(start)) return true;
  }
  return false;
}

}  // namespace

std::optional<std::string> WhyNotAThrowawayPc(std::wstring_view systemTests,
                                              std::wstring_view githubActions,
                                              std::wstring_view runnerEnvironment,
                                              std::wstring_view throwawayVm) {
  if (systemTests != L"1") {
    return std::string(
        "PDFT_SYSTEM_TESTS is not 1: these tests install and uninstall for real, so they run "
        "only on a throw-away PC that asks for them (the CI job system-x64)");
  }
  const bool githubHosted = githubActions == L"true" && runnerEnvironment == L"github-hosted";
  if (!githubHosted && throwawayVm != L"1") {
    return std::string(
        "neither GITHUB_ACTIONS=true with RUNNER_ENVIRONMENT=github-hosted (a runner GitHub "
        "hosts; a self-hosted runner may be a PC someone uses) nor "
        "PDFT_SYSTEM_TESTS_THROWAWAY_VM=1 (a throw-away virtual machine) says that this PC is "
        "thrown away after the run, and the tests make and delete local accounts on it (an "
        "administrator and a standard user)");
  }
  return std::nullopt;
}

std::optional<std::string> WhyNoTestAccount(const AccountGuard& guard) {
  if (std::optional<std::string> why = WhyNotAThrowawayPc(
          guard.systemTests, guard.githubActions, guard.runnerEnvironment, guard.throwawayVm)) {
    return why;
  }
  if (!guard.elevated) return std::string("not elevated: only an administrator can make it");
  if (!guard.realRegistry) {
    return std::string(
        "the registry may be virtualized here (or a parent runs in an app package), so the tests "
        "would not change the real PC");
  }
  if (guard.productFound) {
    return std::string("1.x or 2.x is installed here, which the tests would change or remove");
  }
  return std::nullopt;
}

std::wstring TestAccountName(const std::array<BYTE, 3>& random) {
  wchar_t hex[8] = {};
  swprintf_s(hex, L"%02x%02x%02x", static_cast<unsigned>(random[0]),
             static_cast<unsigned>(random[1]), static_cast<unsigned>(random[2]));
  return std::wstring(kNamePrefix) + hex;
}

bool IsTestAccount(std::wstring_view name, std::wstring_view comment) noexcept {
  if (comment != kTestAccountComment) return false;
  if (name.size() != kNamePrefix.size() + kNameHexDigits || !name.starts_with(kNamePrefix)) {
    return false;
  }
  for (const wchar_t c : name.substr(kNamePrefix.size())) {
    if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
  }
  return true;
}

std::optional<std::wstring> PasswordFromRandom(std::span<const BYTE> random,
                                               std::wstring_view accountName) {
  const std::wstring alphabet =
      std::wstring(kUpper) + std::wstring(kLower) + std::wstring(kDigits) + std::wstring(kSymbols);
  // Bytes from the largest multiple of the alphabet's size up are skipped: every character is
  // then picked by as many byte values as every other.
  const size_t usable = 256 - 256 % alphabet.size();
  std::wstring password;
  password.reserve(kPasswordLength);  // never moved while it grows: nothing to wipe elsewhere
  for (const BYTE value : random) {
    if (password.size() == kPasswordLength) break;
    if (value >= usable) continue;
    password.push_back(alphabet[value % alphabet.size()]);
  }
  const auto has = [&password](std::wstring_view kind) {
    return password.find_first_of(kind.data(), 0, kind.size()) != std::wstring::npos;
  };
  if (password.size() < kPasswordLength || !has(kUpper) || !has(kLower) || !has(kDigits) ||
      !has(kSymbols) || HoldsTheName(password, accountName)) {
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    return std::nullopt;
  }
  return std::optional<std::wstring>(std::move(password));  // moved: no copy left behind
}

std::vector<GTestCase> ReadGTestCases(std::string_view output) {
  std::vector<GTestCase> cases;
  bool reasonNext = false;
  for (size_t start = 0; start < output.size();) {
    size_t end = output.find('\n', start);
    if (end == std::string_view::npos) end = output.size();
    std::string_view line = output.substr(start, end - start);
    start = end + 1;
    if (line.ends_with('\r')) line.remove_suffix(1);
    // The lists after this line name tests again, the end lines' way but without their time.
    if (line.starts_with("[==========] ") && line.find(" ran.") != std::string_view::npos) break;
    // Only a test that has not ended yet takes a result.
    GTestCase* running = !cases.empty() && cases.back().result == GTestCase::Result::kUnfinished
                             ? &cases.back()
                             : nullptr;
    if (reasonNext) {
      reasonNext = false;
      if (running && !line.starts_with('[')) {
        running->skipReason = line;
        continue;
      }
    }
    if (const std::optional<std::string_view> name = NameAfter(line, "[ RUN      ] ")) {
      cases.push_back(GTestCase{std::string(*name)});
      continue;
    }
    if (line.ends_with(": Skipped")) {
      reasonNext = true;
      continue;
    }
    struct EndTag {
      std::string_view tag;
      GTestCase::Result result;
    };
    for (const EndTag& ending : {EndTag{"[       OK ] ", GTestCase::Result::kPassed},
                                 EndTag{"[  FAILED  ] ", GTestCase::Result::kFailed},
                                 EndTag{"[  SKIPPED ] ", GTestCase::Result::kSkipped}}) {
      const std::optional<std::string_view> name = NameAfter(line, ending.tag);
      if (name && running && running->name == *name) running->result = ending.result;
    }
  }
  return cases;
}

std::vector<std::string> StandardAccountTaskTestProblems(const std::vector<GTestCase>& cases,
                                                         DWORD exitCode) {
  std::vector<std::string> problems;
  if (exitCode != 0) problems.push_back("the test program exited " + std::to_string(exitCode));
  bool writing = false;
  bool folder = false;
  for (const GTestCase& test : cases) {
    if (test.name.starts_with("TaskWriting.")) {
      writing = true;
      if (test.result == GTestCase::Result::kPassed) continue;
      if (test.result == GTestCase::Result::kSkipped && test.name == kSessionTaskTest &&
          StartsWithOneOf(test.skipReason, kNoSessionSkips)) {
        continue;
      }
      problems.push_back(test.name + " " + ResultText(test) +
                         ": every TaskWriting test must pass for a standard user");
    } else if (test.name.starts_with("TaskFolderElevated.")) {
      folder = true;
      if (test.result == GTestCase::Result::kSkipped &&
          test.skipReason.starts_with(kNeedsElevationSkip)) {
        continue;
      }
      problems.push_back(test.name + " " + ResultText(test) +
                         ": the protected task folder's tests must skip for a standard user, who "
                         "is not elevated");
    } else {
      problems.push_back(test.name + " ran, which the filter leaves out");
    }
  }
  if (!writing) problems.push_back("no TaskWriting test ran");
  if (!folder) problems.push_back("no TaskFolderElevated test ran");
  for (const char* required : kStandardAccountRequiredTests) {
    bool ran = false;
    for (const GTestCase& test : cases) ran = ran || test.name == required;
    if (!ran) problems.push_back(std::string(required) + " did not run");
  }
  return problems;
}

std::wstring_view PendingRenamePath(std::wstring_view entry) noexcept {
  if (entry.starts_with(L'*')) {
    entry.remove_prefix(1);
    while (!entry.empty() && entry.front() >= L'0' && entry.front() <= L'9') entry.remove_prefix(1);
  }
  if (entry.starts_with(L"\\??\\")) entry.remove_prefix(4);
  return entry;
}

}  // namespace pdft::systest
