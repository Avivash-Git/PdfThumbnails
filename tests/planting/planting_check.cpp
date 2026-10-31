// pdft-planting-check (test only; runs as gate.planting): proves that the app's executables do
// not load DLLs planted in their own folder.
//
//   pdft-planting-check --canary <dll> --exe <path>... --plant <dll name>...
//
// 1. Makes a fresh folder under %TEMP%\pdft-tests, copies each executable into it, and the canary
//    DLL (canary.cpp) under every --plant name (the list is in cmake/Gates.cmake).
// 2. Positive control: loads the first planted copy itself, by full path, and expects its marker.
//    A canary that cannot load at all would make the check pass for the wrong reason. Only an
//    Application Control refusal (Smart App Control may refuse the unsigned DLL) is a SKIP; any
//    other load error means the check itself is broken: FAIL.
// 3. Runs each executable with --version and with --help, in that folder, and expects exit code 0
//    and the right output. When this program runs elevated (CI's runners), also with
//    "--status --no-live-test", which reads the process context (the process tree, the account,
//    WMI's registry provider) and so loads the system DLLs that most planted names aim at. Not
//    in a normal developer run: there --status would write the user's real log, while an
//    elevated process never logs.
// 4. Fails if any canary left a marker: a planted DLL was loaded.
//
// Exit: 0 passed, 1 failed, 77 skipped: Application Control (Smart App Control, WDAC) blocked the
// canary or an executable, and the runs before the block found nothing wrong. Nothing is started
// again after a block. The folder is deleted on every path (unless PDFT_KEEP_TEMP=1).

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "medium_il.h"
#include "project_config.h"
#include "temp_dir.h"

namespace {

constexpr int kPassed = 0;
constexpr int kFailed = 1;
constexpr int kSkipped = 77;
// The first start of a new file under Smart App Control can take about 10 s (a cloud lookup).
constexpr DWORD kRunTimeoutMs = 120000;
constexpr wchar_t kCanaryDirEnv[] = L"PDFT_CANARY_DIR";

struct Options {
  std::wstring canary;
  std::vector<std::wstring> exes;
  std::vector<std::wstring> plants;
};

HRESULT LastError() {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

std::wstring FileName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::string Hex(unsigned long value) {
  char text[16] = {};
  std::snprintf(text, sizeof(text), "0x%08lX", value);
  return text;
}

std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}

// The bytes of a (small) file; empty if it cannot be read.
std::string ReadBytes(const std::wstring& path) {
  std::string bytes;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return bytes;
  char buffer[4096];
  DWORD read = 0;
  while (bytes.size() < (1u << 20) && ReadFile(file, buffer, sizeof(buffer), &read, nullptr) &&
         read > 0) {
    bytes.append(buffer, read);
  }
  CloseHandle(file);
  return bytes;
}

// The canary markers in |folder|, each with the program that loaded the DLL.
std::vector<std::wstring> ListMarkers(const std::wstring& folder) {
  std::vector<std::wstring> found;
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW((folder + L"\\*.loaded").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return found;
  do {
    const std::wstring marker = data.cFileName;
    const std::string bytes = ReadBytes(folder + L"\\" + marker);
    const std::wstring program(reinterpret_cast<const wchar_t*>(bytes.data()),
                               bytes.size() / sizeof(wchar_t));
    found.push_back(marker + L" (loaded by " + (program.empty() ? L"?" : program) + L")");
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return found;
}

struct Run {
  DWORD exitCode = 0;
  std::string output;  // standard output and standard error together
};

// Starts |exe| with one |argument| in |folder| (its working directory), standard output and
// error going to |outputFile|, and waits for it.
HRESULT RunProgram(const std::wstring& exe, const std::wstring& argument,
                   const std::wstring& folder, const std::wstring& outputFile, Run* run) {
  SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
  HANDLE output = CreateFileW(outputFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) return LastError();

  // The child inherits this one handle and nothing else.
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<BYTE> attributes(size);
  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
    const HRESULT hr = LastError();
    CloseHandle(output);
    return hr;
  }
  HRESULT hr = S_OK;
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 &output, sizeof(output), nullptr, nullptr)) {
    hr = LastError();
  }
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
  startup.StartupInfo.hStdOutput = output;
  startup.StartupInfo.hStdError = output;

  std::wstring commandLine = pdft::test::QuoteArgument(exe) + L" " + argument;
  PROCESS_INFORMATION process = {};
  if (SUCCEEDED(hr) && !CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                       folder.c_str(), &startup.StartupInfo, &process)) {
    hr = LastError();
  }
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  CloseHandle(output);
  if (FAILED(hr)) return hr;

  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, kRunTimeoutMs);
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 10000);
    CloseHandle(process.hProcess);
    return HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
  }
  if (!GetExitCodeProcess(process.hProcess, &run->exitCode)) hr = LastError();
  CloseHandle(process.hProcess);
  run->output = ReadBytes(outputFile);
  return hr;
}

// Elevated the way the app decides it (ProcessContext::elevated): Administrators enabled, or an
// integrity level above Medium. The executables inherit this process's token, so they decide
// the same. An unreadable token counts as not elevated: the commands that need elevation to stay
// out of the user's log are then left out.
bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token)) return false;
  const bool administrators = pdft::test::TokenHasEnabledAdministrators(token);
  const DWORD integrity = pdft::test::TokenIntegrityRid(token);
  CloseHandle(token);
  return administrators ||
         (integrity != pdft::test::kUnknownIntegrity && integrity > SECURITY_MANDATORY_MEDIUM_RID);
}

// One command line for each executable, and the exit codes it may end with.
struct Command {
  const wchar_t* arguments;
  std::vector<DWORD> exitCodes;
};

bool ParseOptions(int argc, wchar_t** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring name = argv[i];
    if (i + 1 >= argc) return false;
    const std::wstring value = argv[++i];
    if (name == L"--canary") {
      options->canary = value;
    } else if (name == L"--exe") {
      options->exes.push_back(value);
    } else if (name == L"--plant" && value.find_first_of(L"\\/:") == std::wstring::npos) {
      options->plants.push_back(value);
    } else {
      return false;
    }
  }
  return !options->canary.empty() && !options->exes.empty() && !options->plants.empty();
}

int Check(const Options& options) {
  pdft::test::TempDir dir(L"planting");
  const std::wstring app = dir.File(L"app");
  const std::wstring markers = dir.File(L"markers");
  if (!dir.ok() || !CreateDirectoryW(app.c_str(), nullptr) ||
      !CreateDirectoryW(markers.c_str(), nullptr)) {
    std::printf("FAIL: cannot create the folders under %%TEMP%%\\pdft-tests (error %lu)\n",
                GetLastError());
    return kFailed;
  }

  std::vector<std::wstring> exes;
  for (const std::wstring& source : options.exes) {
    const std::wstring copy = app + L"\\" + FileName(source);
    if (!CopyFileW(source.c_str(), copy.c_str(), TRUE)) {
      std::printf("FAIL: cannot copy %ls (error %lu)\n", source.c_str(), GetLastError());
      return kFailed;
    }
    exes.push_back(copy);
  }
  for (const std::wstring& name : options.plants) {
    if (!CopyFileW(options.canary.c_str(), (app + L"\\" + name).c_str(), TRUE)) {
      std::printf("FAIL: cannot plant %ls (error %lu)\n", name.c_str(), GetLastError());
      return kFailed;
    }
  }
  if (!SetEnvironmentVariableW(kCanaryDirEnv, markers.c_str())) {
    std::printf("FAIL: cannot set %ls (error %lu)\n", kCanaryDirEnv, GetLastError());
    return kFailed;
  }

  // Positive control: the canary must be able to load and leave its marker here.
  const std::wstring control = app + L"\\" + options.plants.front();
  HMODULE module = LoadLibraryExW(control.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  const DWORD loadError = module ? ERROR_SUCCESS : GetLastError();
  if (module) {
    FreeLibrary(module);
    std::printf("FAIL: the canary %ls loaded instead of refusing to\n", control.c_str());
    return kFailed;
  }
  std::vector<std::wstring> found = ListMarkers(markers);
  if (found.empty()) {
    if (pdft::test::IsApplicationControlBlock(HRESULT_FROM_WIN32(loadError))) {
      std::printf(
          "SKIP: Application Control blocked the canary DLL (error %lu), so planting cannot be "
          "checked here\n",
          loadError);
      return kSkipped;
    }
    if (loadError == ERROR_DLL_INIT_FAILED) {
      std::printf("FAIL: the canary ran but wrote no marker into %ls: the check itself is broken\n",
                  markers.c_str());
      return kFailed;
    }
    // Without Application Control (CI) nothing should stop the canary from loading: a wrong
    // architecture, a missing dependency or no access means this check would prove nothing.
    std::printf("FAIL: positive control broken: the canary DLL could not be loaded (error %lu)\n",
                loadError);
    return kFailed;
  }
  const std::wstring expected = options.plants.front() + L".loaded";
  if (found.size() != 1 || found.front().rfind(expected, 0) != 0) {
    std::printf("FAIL: the positive control left unexpected markers:\n");
    for (const std::wstring& marker : found) std::printf("  %ls\n", marker.c_str());
    return kFailed;
  }
  if (!DeleteFileW((markers + L"\\" + expected).c_str())) {
    std::printf("FAIL: cannot delete the control marker (error %lu)\n", GetLastError());
    return kFailed;
  }
  std::printf("ok: positive control: %ls, loaded by full path, wrote its marker (load error %lu)\n",
              options.plants.front().c_str(), loadError);

  // The executables, each command once. --version must print exactly the version; the others
  // must print something (only the first line is looked at).
  const std::string version = Utf8(std::wstring(pdft::kProductName) + L" " + pdft::kProductVersion +
                                   L" (" + pdft::kArch + L")\r\n");
  std::vector<Command> commands = {{L"--version", {0}}, {L"--help", {0}}};
  // Elevated only: a non-elevated --status would write the user's real log. 0 healthy, 1
  // problems, 6 not installed (a CI runner).
  const bool elevated = IsElevated();
  if (elevated) commands.push_back({L"--status --no-live-test", {0, 1, 6}});
  std::printf("%s: %s\n", elevated ? "elevated" : "not elevated",
              elevated ? "also running --status --no-live-test"
                       : "--version and --help only (--status would write the user's log)");
  std::vector<std::string> problems;
  int runs = 0;
  bool blocked = false;
  for (const std::wstring& exe : exes) {
    for (const Command& command : commands) {
      Run run;
      const std::wstring output = dir.File(L"output-" + std::to_wstring(runs++) + L".txt");
      const HRESULT hr = RunProgram(exe, command.arguments, app, output, &run);
      if (FAILED(hr) && pdft::test::IsApplicationControlBlock(hr)) {
        std::printf("Application Control blocked %ls (0x%08lX); not starting anything else\n",
                    exe.c_str(), static_cast<unsigned long>(hr));
        blocked = true;
        break;
      }
      const std::string label = Utf8(FileName(exe)) + " " + Utf8(command.arguments);
      if (FAILED(hr)) {
        problems.push_back(label + ": cannot run it (" + Hex(static_cast<unsigned long>(hr)) + ")");
        continue;
      }
      const std::string firstLine = run.output.substr(0, run.output.find_first_of("\r\n"));
      std::printf("%s -> exit %s: %s\n", label.c_str(), Hex(run.exitCode).c_str(),
                  firstLine.c_str());
      const bool expectedCode = std::find(command.exitCodes.begin(), command.exitCodes.end(),
                                          run.exitCode) != command.exitCodes.end();
      if (!expectedCode) {
        problems.push_back(label + ": unexpected exit code " + Hex(run.exitCode));
      } else if (std::wstring(command.arguments) == L"--version" ? run.output != version
                                                                 : firstLine.empty()) {
        problems.push_back(label + ": unexpected output '" + run.output + "'");
      }
    }
    if (blocked) break;
  }

  // What the runs before a block showed still counts: a planted DLL that was loaded is a failure.
  found = ListMarkers(markers);
  for (const std::wstring& marker : found) {
    problems.push_back("planted DLL loaded: " + Utf8(marker));
  }
  if (!problems.empty()) {
    std::printf("FAIL:\n");
    for (const std::string& problem : problems) std::printf("  %s\n", problem.c_str());
    return kFailed;
  }
  if (blocked) {
    std::printf("SKIP: not every executable could run, so the check proves nothing\n");
    return kSkipped;
  }
  std::printf("ok: %d runs of %zu executables, none of %zu planted DLLs was loaded\n", runs,
              exes.size(), options.plants.size());
  return kPassed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
      std::fputs(
          "usage: pdft-planting-check --canary <dll> --exe <path>... --plant <dll name>...\n",
          stderr);
      return 2;
    }
    const int result = Check(options);
    std::fflush(stdout);
    return result;
  } catch (...) {
    std::printf("FAIL: unexpected exception\n");
    return kFailed;
  }
}
