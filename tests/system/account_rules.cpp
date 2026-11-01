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
        "thrown away after the run, and the tests make and delete a local administrator account "
        "on it");
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

}  // namespace pdft::systest
