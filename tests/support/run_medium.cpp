// pdft-run-medium: runs a command at Medium integrity (test tool; never shipped).
//
//   pdft-run-medium [--label-only] [--] <program> [arguments...]
//       Runs the program with this process's token lowered to Medium integrity and returns its
//       exit code. By default the token is also filtered (Administrators deny-only, no
//       privileges), like a standard user's; --label-only changes only the integrity label.
//       125 = the program could not be started; 126 = Application Control (Smart App Control,
//       WDAC) blocked it.
//   pdft-run-medium --check-token
//       Prints this process's integrity level, whether the Administrators group is enabled, the
//       elevation type, and the token's owner and default DACL as SDDL; exit 0 when it runs at
//       Medium without Administrators, else 3.
//   pdft-run-medium --exit <n>
//       Exits with <n> (a harmless target for tests).

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "medium_il.h"

namespace {

int Usage() {
  std::fputs(
      "usage:\n"
      "  pdft-run-medium [--label-only] [--] <program> [arguments...]\n"
      "  pdft-run-medium --check-token\n"
      "  pdft-run-medium --exit <n>\n"
      "exit codes: the program's, or 125 = it could not be started, 126 = Application Control\n"
      "blocked it\n",
      stderr);
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return Usage();
  const std::wstring first = argv[1];
  if (first == L"--check-token") return argc == 2 ? pdft::test::ReportOwnToken() : Usage();
  if (first == L"--exit") {
    if (argc != 3) return Usage();
    wchar_t* end = nullptr;
    const long code = std::wcstol(argv[2], &end, 10);
    return end && *end == L'\0' ? static_cast<int>(code) : Usage();
  }

  auto kind = pdft::test::MediumToken::kFiltered;
  int i = 1;
  if (i < argc && std::wstring(argv[i]) == L"--label-only") {
    kind = pdft::test::MediumToken::kLabelOnly;
    ++i;
  }
  if (i < argc && std::wstring(argv[i]) == L"--") ++i;
  if (i >= argc) return Usage();
  std::wstring commandLine;
  for (; i < argc; ++i) {
    if (!commandLine.empty()) commandLine += L' ';
    commandLine += pdft::test::QuoteArgument(argv[i]);
  }

  HANDLE token = nullptr;
  HRESULT hr = pdft::test::CreateMediumTokenForSelf(kind, &token);
  DWORD code = 125;
  if (SUCCEEDED(hr)) {
    hr = pdft::test::RunWithToken(token, std::wstring(), commandLine, &code);
    CloseHandle(token);
  }
  if (FAILED(hr)) {
    std::fprintf(stderr, "pdft-run-medium: cannot run the program at Medium integrity: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    return pdft::test::IsApplicationControlBlock(hr) ? 126 : 125;
  }
  return static_cast<int>(code);
}
