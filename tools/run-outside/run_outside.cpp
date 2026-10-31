// pdft-run-outside: runs a program as the signed-in user, in that user's normal desktop session,
// through a temporary scheduled task. Developer tool; never shipped.
//
//   pdft-run-outside [--timeout <seconds>] -- <program.exe> [arguments...]
//
// A terminal that runs inside an app container or a sandbox can get a private copy of the user's
// registry and AppData, and so does every program it starts. Task Scheduler starts its tasks in
// the user's own context, outside such containers, so a program started this way sees the same
// registry and Explorer as the rest of the session. Tests use it for checks that must see the
// real thing.
//
// How it works (no shell anywhere, so nothing in the arguments is ever interpreted):
//   1. This process (the caller) makes a private folder %TEMP%\<prefix><run id>, which only this
//      user and SYSTEM can open, and writes the request into it: the program's full path, its
//      arguments and the caller's current folder.
//   2. It registers the task <dev task folder>\run-<run id> for the current user (interactive
//      token, limited rights, normal priority, a time limit a little over --timeout, for when
//      the caller is gone), whose only action is this executable with "--child <run id>",
//      starts it and waits; at --timeout it stops the task itself.
//   3. That second copy (the child) checks that the run folder and the request are ours, reads
//      and deletes the request, starts the program with CreateProcessW with its standard output
//      and error going to a file in the run folder, waits, records the exit code next to it and
//      exits with that code.
//   4. The caller prints the output, deletes the task, the task folder when no other run uses
//      it, and the run folder (on every path, Ctrl+C included), and returns the program's exit
//      code.
// The program gets the user's own environment (as Task Scheduler builds it, not the caller's),
// the caller's current folder, no standard input and no console window. It is not looked up on
// PATH: give its path. A pdft-run-outside that is killed can leave its task behind; delete it
// with Task Scheduler.
//
// Exit codes: the program's, or 124 = it ran longer than --timeout (default 120 s) and was
// stopped, 125 = pdft-run-outside failed (see standard error), 126 = Application Control (Smart
// App Control or an App Control policy) blocked it, 127 = it could not be started for another
// reason (both explained in its output), 130 = interrupted.

#include <windows.h>

#include <aclapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "medium_il.h"
#include "project_config.h"
#include "run_outside_core.h"
#include "temp_dir.h"

namespace {

using Microsoft::WRL::ComPtr;
namespace outside = pdft::outside;

constexpr DWORD kPollMs = 200;
// A started task leaves the "ready" state at once; one that has neither run nor become queued
// or running after this long will not start.
constexpr ULONGLONG kStartGraceMs = 15000;
// Task Scheduler's default priority (7) is below normal, and a program started from it would
// inherit that; 4 to 6 are normal, as for a program started from a terminal.
constexpr int kNormalTaskPriority = 5;
// Task Scheduler's time limit is --timeout plus this. The caller's own deadline must come first:
// Task Scheduler starts its clock inside IRegisteredTask::Run, before the caller starts its own,
// and when its limit ends the task first the run cannot be told from one that ended by itself.
// Its limit is only the backstop for when the caller is gone; this covers the caller's wait of up
// to 10 s for a stopped task to end, with room to spare.
constexpr DWORD kTaskLimitMarginSeconds = 30;
static_assert(outside::kMaxTimeoutSeconds <= MAXDWORD - kTaskLimitMarginSeconds);
constexpr size_t kMaxConsoleOutput = 64u << 20;
constexpr wchar_t kTaskDescription[] =
    L"Temporary task of pdft-run-outside (developer tool): runs one program and is deleted "
    L"when it ends.";

// Set by the console control handler: stop waiting (Ctrl+C, Ctrl+Break, the console closing).
HANDLE g_cancel = nullptr;
// Set once the task and the run folder are gone, so a closing console can wait for that.
HANDLE g_cleanedUp = nullptr;

struct HandleCloser {
  void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// CreateFileW's result as an owner (empty on failure).
UniqueHandle Owned(HANDLE handle) {
  return UniqueHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

struct LocalFreer {
  void operator()(void* memory) const noexcept { LocalFree(memory); }
};
using UniqueLocal = std::unique_ptr<void, LocalFreer>;

class Bstr {
 public:
  explicit Bstr(std::wstring_view text)
      : value_(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  ~Bstr() { SysFreeString(value_); }
  BSTR get() const { return value_; }

 private:
  BSTR value_;
};

VARIANT Empty() {
  VARIANT value;
  VariantInit(&value);
  return value;
}

// GetLastError() as an HRESULT; never S_OK, because a few functions fail without setting it.
HRESULT LastError() {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

// "0x80070005 (Access is denied)".
std::wstring Describe(HRESULT hr) {
  wchar_t hex[16];
  swprintf_s(hex, L"0x%08lX", static_cast<unsigned long>(hr));
  std::wstring text = hex;
  wchar_t* message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
  UniqueLocal owner(message);
  if (length > 0 && message) {
    std::wstring_view view(message, length);
    while (!view.empty() && (view.back() == L'\r' || view.back() == L'\n' || view.back() == L' ' ||
                             view.back() == L'.')) {
      view.remove_suffix(1);
    }
    if (!view.empty()) text += L" (" + std::wstring(view) + L")";
  }
  return text;
}

// ---- output ----------------------------------------------------------------------------------

bool IsConsole(HANDLE handle) {
  DWORD mode = 0;
  return GetFileType(handle) == FILE_TYPE_CHAR && GetConsoleMode(handle, &mode);
}

std::string Utf8(std::wstring_view text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? static_cast<size_t>(size) : 0, '\0');
  if (size > 0) {
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
  }
  return out;
}

// Invalid sequences become U+FFFD: the program's output may be in any encoding.
std::wstring Wide(std::string_view text) {
  if (text.empty()) return std::wstring();
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(size > 0 ? static_cast<size_t>(size) : 0, L'\0');
  if (size > 0) {
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  }
  return out;
}

bool WriteAll(HANDLE handle, const void* data, size_t size) {
  const BYTE* next = static_cast<const BYTE*>(data);
  while (size > 0) {
    const DWORD chunk = static_cast<DWORD>(size < (1u << 20) ? size : (1u << 20));
    DWORD written = 0;
    if (!WriteFile(handle, next, chunk, &written, nullptr) || written == 0) return false;
    next += written;
    size -= written;
  }
  return true;
}

// To a console as UTF-16 (any code page shows it), elsewhere as UTF-8.
void WriteText(HANDLE handle, std::wstring_view text) {
  if (!handle || handle == INVALID_HANDLE_VALUE || text.empty()) return;
  if (!IsConsole(handle)) {
    const std::string bytes = Utf8(text);
    WriteAll(handle, bytes.data(), bytes.size());
    return;
  }
  while (!text.empty()) {
    const DWORD chunk = static_cast<DWORD>(text.size() < 16384 ? text.size() : 16384);
    DWORD written = 0;
    if (!WriteConsoleW(handle, text.data(), chunk, &written, nullptr) || written == 0) return;
    text.remove_prefix(written);
  }
}

// For the last-resort messages in catch handlers: allocates nothing, so it cannot throw.
void WriteRawError(const char* text) noexcept {
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(std::strlen(text)), &written,
            nullptr);
}

// One line on standard error, after the tool's name.
void Message(std::wstring_view text) {
  WriteText(GetStdHandle(STD_ERROR_HANDLE), L"pdft-run-outside: " + std::wstring(text) + L"\r\n");
}

void PrintUsage(HANDLE handle) {
  WriteText(
      handle,
      L"usage: pdft-run-outside [--timeout <seconds>] -- <program.exe> [arguments...]\r\n"
      L"       pdft-run-outside --help\r\n"
      L"\r\n"
      L"Runs the program as the signed-in user, in that user's normal desktop session, through "
      L"a\r\n"
      L"temporary scheduled task: it sees the real registry and Explorer even when this "
      L"terminal\r\n"
      L"runs inside an app container or a sandbox. The program gets exactly these arguments (no\r\n"
      L"shell is involved), this terminal's current folder, the user's own environment, no\r\n"
      L"standard input and no console window. Its standard output and error are printed when it\r\n"
      L"ends. Give the program's path; it is not looked up on PATH.\r\n"
      L"\r\n"
      L"  --timeout <seconds>  stop the program after this long (1 to 86400, default 120)\r\n"
      L"\r\n"
      L"Exit codes: the program's, or 124 = it ran longer than --timeout and was stopped,\r\n"
      L"125 = pdft-run-outside failed, 126 = Application Control blocked it, 127 = it could not\r\n"
      L"be started, 130 = interrupted.\r\n");
}

// ---- paths and files -------------------------------------------------------------------------

// Calls |get(buffer, size)| (a Win32 function that returns the length it needs when the buffer
// is too small, the length it wrote otherwise, 0 on failure) with a growing buffer.
template <typename Get>
HRESULT GrowingString(Get get, std::wstring* out) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD length = get(buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return LastError();
    if (length < buffer.size()) {
      buffer.resize(length);
      *out = std::move(buffer);
      return S_OK;
    }
    buffer.resize(static_cast<size_t>(length) + 1);
  }
  return E_UNEXPECTED;
}

// This executable's full path.
HRESULT OwnPath(std::wstring* path) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return LastError();
    if (length < buffer.size()) {
      buffer.resize(length);
      *path = std::move(buffer);
      return S_OK;
    }
    if (buffer.size() >= 32768) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    buffer.resize(buffer.size() * 2);
  }
}

HRESULT FullPath(const std::wstring& path, std::wstring* full) {
  return GrowingString(
      [&path](wchar_t* buffer, DWORD size) {
        return GetFullPathNameW(path.c_str(), size, buffer, nullptr);
      },
      full);
}

HRESULT CurrentFolder(std::wstring* folder) {
  return GrowingString(
      [](wchar_t* buffer, DWORD size) { return GetCurrentDirectoryW(size, buffer); }, folder);
}

// The user's temporary folder, ending with a backslash.
HRESULT TempFolder(std::wstring* folder) {
  return GrowingString([](wchar_t* buffer, DWORD size) { return GetTempPathW(size, buffer); },
                       folder);
}

// What |handle| really is, links resolved ("\\?\C:\...").
HRESULT FinalPath(HANDLE handle, std::wstring* path) {
  return GrowingString(
      [handle](wchar_t* buffer, DWORD size) {
        return GetFinalPathNameByHandleW(handle, buffer, size,
                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      },
      path);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// This process's user SID, in a buffer of its own.
HRESULT UserSid(std::vector<BYTE>* sid) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return LastError();
  UniqueHandle owner(token);
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  if (!GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) return LastError();
  const PSID user = reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid;
  const BYTE* begin = static_cast<const BYTE*>(user);
  sid->assign(begin, begin + GetLengthSid(user));
  return S_OK;
}

HRESULT SidString(PSID sid, std::wstring* text) {
  wchar_t* converted = nullptr;
  if (!ConvertSidToStringSidW(sid, &converted)) return LastError();
  UniqueLocal owner(converted);
  *text = converted;
  return S_OK;
}

// "DOMAIN\user", the form Task Scheduler takes for a principal.
HRESULT AccountName(PSID sid, std::wstring* account) {
  wchar_t name[256];
  wchar_t domain[256];
  DWORD nameLength = ARRAYSIZE(name);
  DWORD domainLength = ARRAYSIZE(domain);
  SID_NAME_USE use = SidTypeUnknown;
  if (!LookupAccountSidW(nullptr, sid, name, &nameLength, domain, &domainLength, &use)) {
    return LastError();
  }
  *account = std::wstring(domain) + L"\\" + name;
  return S_OK;
}

// Owner = the user; only the user and SYSTEM have access, nothing is inherited from %TEMP%.
HRESULT PrivateDescriptor(const std::wstring& sid, bool folder, UniqueLocal* descriptor) {
  const std::wstring inherit = folder ? L"OICI" : L"";
  const std::wstring sddl =
      L"O:" + sid + L"D:P(A;" + inherit + L";FA;;;" + sid + L")(A;" + inherit + L";FA;;;SY)";
  PSECURITY_DESCRIPTOR converted = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            &converted, nullptr)) {
    return LastError();
  }
  descriptor->reset(converted);
  return S_OK;
}

HRESULT WriteNewFile(const std::wstring& path, const std::vector<BYTE>& bytes,
                     PSECURITY_DESCRIPTOR descriptor) {
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
  UniqueHandle file = Owned(CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return LastError();
  return WriteAll(file.get(), bytes.data(), bytes.size()) ? S_OK : LastError();
}

// Opens a plain file of the run folder without following a link; empty if there is none.
UniqueHandle OpenRunFile(const std::wstring& path) {
  UniqueHandle file = Owned(CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!file || !GetFileInformationByHandle(file.get(), &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
    return UniqueHandle();
  }
  return file;
}

// Up to |max| bytes; false when the file is larger or cannot be read.
bool ReadAll(HANDLE file, size_t max, std::vector<BYTE>* bytes) {
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) > max) {
    return false;
  }
  bytes->resize(static_cast<size_t>(size.QuadPart));
  size_t done = 0;
  while (done < bytes->size()) {
    DWORD read = 0;
    const DWORD chunk =
        static_cast<DWORD>(bytes->size() - done < (1u << 20) ? bytes->size() - done : (1u << 20));
    if (!ReadFile(file, bytes->data() + done, chunk, &read, nullptr) || read == 0) return false;
    done += read;
  }
  return true;
}

// ---- the child: started by the task ---------------------------------------------------------

// Opens |path| without following a link and checks that it is what the caller made: a folder,
// or a file with no other name (hard link), owned by |owner|, whose final path is |expected|.
HRESULT OpenOwned(const std::wstring& path, bool folder, DWORD access, DWORD share, PSID owner,
                  const std::wstring& expected, UniqueHandle* handle, std::wstring* problem) {
  UniqueHandle opened = Owned(CreateFileW(
      path.c_str(), access | READ_CONTROL | FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | (folder ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
  if (!opened) {
    const HRESULT hr = LastError();
    *problem = L"cannot open " + path + L": " + Describe(hr);
    return hr;
  }
  const HRESULT notOurs = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!GetFileInformationByHandle(opened.get(), &info)) {
    const HRESULT hr = LastError();
    *problem = L"cannot read the attributes of " + path + L": " + Describe(hr);
    return hr;
  }
  const bool isFolder = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || isFolder != folder ||
      (!folder && info.nNumberOfLinks != 1)) {
    *problem = path + L" is a link or not a plain " + (folder ? L"folder" : L"file");
    return notOurs;
  }
  PSID fileOwner = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD error = GetSecurityInfo(opened.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                      &fileOwner, nullptr, nullptr, nullptr, &descriptor);
  UniqueLocal descriptorOwner(descriptor);
  if (error != ERROR_SUCCESS) {
    *problem = L"cannot read the owner of " + path + L": " + Describe(HRESULT_FROM_WIN32(error));
    return HRESULT_FROM_WIN32(error);
  }
  if (!fileOwner || !EqualSid(fileOwner, owner)) {
    *problem = path + L" does not belong to this user";
    return notOurs;
  }
  std::wstring resolved;
  const HRESULT hr = FinalPath(opened.get(), &resolved);
  if (FAILED(hr)) {
    *problem = L"cannot resolve " + path + L": " + Describe(hr);
    return hr;
  }
  if (!SamePath(resolved, expected)) {
    *problem = path + L" leads to " + resolved + L", not to " + expected;
    return notOurs;
  }
  *handle = std::move(opened);
  return S_OK;
}

// Reads and deletes the request file in |folder| (a final path) after checking it.
HRESULT TakeRequest(const std::wstring& folder, PSID user, outside::Request* request,
                    std::wstring* problem) {
  const std::wstring path = folder + L"\\" + outside::kRequestFile;
  UniqueHandle file;
  HRESULT hr = OpenOwned(path, false, GENERIC_READ | DELETE, 0, user, path, &file, problem);
  if (FAILED(hr)) return hr;
  std::vector<BYTE> bytes;
  if (!ReadAll(file.get(), outside::kMaxRequestBytes, &bytes)) {
    hr = LastError();
    *problem = L"cannot read " + path + L" (or it is too large): " + Describe(hr);
    return hr;
  }
  FILE_DISPOSITION_INFO remove = {TRUE};
  if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &remove, sizeof(remove))) {
    hr = LastError();
    *problem = L"cannot delete " + path + L": " + Describe(hr);
    return hr;
  }
  file.reset();
  hr = outside::DecodeRequest(bytes, request);
  if (FAILED(hr)) *problem = path + L" is not a request of this version of pdft-run-outside";
  return hr;
}

int RunChild(const std::wstring& runId) {
  // Until the run folder is known to be ours there is nowhere to report to: the caller then
  // explains exit code 125 without a result record.
  std::vector<BYTE> user;
  std::wstring temp;
  if (FAILED(UserSid(&user)) || FAILED(TempFolder(&temp))) return outside::kExitFailed;
  const PSID sid = user.data();
  // %TEMP% itself may be a link (the user's choice); what is below it must not be.
  UniqueHandle tempHandle = Owned(CreateFileW(
      temp.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  std::wstring tempFinal;
  if (!tempHandle || FAILED(FinalPath(tempHandle.get(), &tempFinal))) return outside::kExitFailed;
  const std::wstring name = std::wstring(pdft::kDevRunPrefix) + runId;
  const std::wstring folderPath = tempFinal + L"\\" + name;
  // Held without delete sharing, so the folder cannot be renamed or replaced meanwhile.
  UniqueHandle folder;
  std::wstring problem;
  if (FAILED(OpenOwned(temp + name, true, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, sid, folderPath,
                       &folder, &problem))) {
    return outside::kExitFailed;
  }

  std::wstring sidText;
  UniqueLocal fileDescriptor;
  if (FAILED(SidString(sid, &sidText)) ||
      FAILED(PrivateDescriptor(sidText, false, &fileDescriptor))) {
    return outside::kExitFailed;
  }
  // The program's standard output and error; inheritable, and the caller may read it meanwhile.
  SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), fileDescriptor.get(), TRUE};
  UniqueHandle output =
      Owned(CreateFileW((folderPath + L"\\" + outside::kOutputFile).c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, &inheritable, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!output) return outside::kExitFailed;

  // From here on every outcome is recorded: the caller trusts the record over the task's
  // result, which may also be a Task Scheduler status.
  const auto finish = [&](DWORD code, const std::wstring& text) {
    if (!text.empty()) {
      const std::string line = Utf8(L"pdft-run-outside: " + text + L"\r\n");
      WriteAll(output.get(), line.data(), line.size());
    }
    const std::string record = outside::FormatResultRecord(code);
    SECURITY_ATTRIBUTES plain = {sizeof(plain), fileDescriptor.get(), FALSE};
    UniqueHandle file =
        Owned(CreateFileW((folderPath + L"\\" + outside::kResultFile).c_str(), GENERIC_WRITE, 0,
                          &plain, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file) WriteAll(file.get(), record.data(), record.size());
    return static_cast<int>(code);
  };

  outside::Request request;
  if (FAILED(TakeRequest(folderPath, sid, &request, &problem))) {
    return finish(outside::kExitFailed, problem);
  }
  std::wstring commandLine;
  HRESULT hr = outside::BuildCommandLine(request.program, request.arguments, &commandLine);
  if (FAILED(hr)) return finish(outside::kExitFailed, L"cannot build the command line");

  UniqueHandle input = Owned(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         &inheritable, OPEN_EXISTING, 0, nullptr));
  if (!input) return finish(outside::kExitFailed, L"cannot open NUL: " + Describe(LastError()));

  // The program ends with this process: when the caller stops the task, or Task Scheduler does
  // at the time limit. What the program starts is not in the job and may outlive it (a
  // restarted Explorer, for example, must).
  UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
  if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                       sizeof(limits))) {
    return finish(outside::kExitFailed, L"cannot create a job object: " + Describe(LastError()));
  }

  // The program inherits these two handles and no other.
  HANDLE inherited[2] = {input.get(), output.get()};
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<BYTE> attributeList(size);
  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.get();
  startup.StartupInfo.hStdOutput = output.get();
  startup.StartupInfo.hStdError = output.get();
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeList.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
    return finish(outside::kExitFailed, L"cannot set up the start: " + Describe(LastError()));
  }
  PROCESS_INFORMATION process = {};
  BOOL started =
      UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                inherited, sizeof(inherited), nullptr, nullptr);
  if (started) {
    // Suspended until it is in the job. No console window: this process has no console, and a
    // console program would otherwise get a new one.
    started =
        CreateProcessW(request.program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                       request.currentDirectory.c_str(), &startup.StartupInfo, &process);
  }
  hr = started ? S_OK : LastError();
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  if (FAILED(hr)) {
    if (pdft::test::IsApplicationControlBlock(hr)) {
      return finish(outside::kExitBlocked,
                    L"Application Control (Smart App Control or an App Control policy) blocked " +
                        request.program + L": " + Describe(hr));
    }
    return finish(outside::kExitNotStarted,
                  L"cannot start " + request.program + L": " + Describe(hr));
  }
  UniqueHandle processHandle(process.hProcess);
  UniqueHandle threadHandle(process.hThread);
  std::wstring note;
  if (!AssignProcessToJobObject(job.get(), process.hProcess)) {
    note = L"note: the program is not in a job object (" + Describe(LastError()) +
           L"); it keeps running if the task is stopped";
  }
  ResumeThread(process.hThread);
  threadHandle.reset();
  DWORD code = 0;
  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
      !GetExitCodeProcess(process.hProcess, &code)) {
    return finish(outside::kExitFailed, L"cannot wait for the program: " + Describe(LastError()));
  }
  return finish(code, note);
}

// ---- the caller ------------------------------------------------------------------------------

BOOL WINAPI OnConsoleControl(DWORD type) noexcept {
  SetEvent(g_cancel);
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) return TRUE;
  // The console is closing or the session is ending: Windows ends this process soon after the
  // handler returns, so give the clean-up a few seconds first.
  WaitForSingleObject(g_cleanedUp, 4500);
  return TRUE;
}

bool Cancelled() { return WaitForSingleObject(g_cancel, 0) == WAIT_OBJECT_0; }

// Deletes the run's task, the task folder when nothing else is in it, and the run folder; runs
// once, whichever way the run ends.
class Cleanup {
 public:
  Cleanup() = default;
  Cleanup(const Cleanup&) = delete;
  Cleanup& operator=(const Cleanup&) = delete;
  ~Cleanup() { Run(); }

  void SetRunFolder(const std::wstring& folder) { runFolder_ = folder; }
  void SetTask(ITaskService* service, ITaskFolder* folder, const std::wstring& name) {
    service_ = service;
    folder_ = folder;
    taskName_ = name;
  }

  void Run() noexcept {
    if (done_) return;
    done_ = true;
    try {
      if (folder_) {
        // No task name: the folder was opened (or made) but the task was never registered.
        if (!taskName_.empty()) {
          const HRESULT hr = folder_->DeleteTask(Bstr(taskName_).get(), 0);
          if (FAILED(hr)) {
            Message(L"could not delete the task " + std::wstring(pdft::kDevTaskFolder) + L"\\" +
                    taskName_ + L": " + Describe(hr));
          }
        }
        DeleteTaskFolderIfEmpty();
      }
      if (!runFolder_.empty()) RemoveRunFolder();
    } catch (...) {
      WriteRawError("pdft-run-outside: the clean-up failed (out of memory?)\r\n");
    }
    SetEvent(g_cleanedUp);
  }

 private:
  // Another run may be using the folder; then it stays, and that run removes it.
  void DeleteTaskFolderIfEmpty() {
    ComPtr<IRegisteredTaskCollection> tasks;
    ComPtr<ITaskFolderCollection> folders;
    LONG taskCount = -1;
    LONG folderCount = -1;
    if (FAILED(folder_->GetTasks(TASK_ENUM_HIDDEN, &tasks)) ||
        FAILED(tasks->get_Count(&taskCount)) || FAILED(folder_->GetFolders(0, &folders)) ||
        FAILED(folders->get_Count(&folderCount)) || taskCount != 0 || folderCount != 0) {
      return;
    }
    folder_.Reset();
    ComPtr<ITaskFolder> root;
    HRESULT hr = service_->GetFolder(Bstr(L"\\").get(), &root);
    if (SUCCEEDED(hr)) hr = root->DeleteFolder(Bstr(pdft::kDevTaskFolder).get(), 0);
    if (hr == E_ACCESSDENIED) {
      Message(L"could not delete the empty task folder " + std::wstring(pdft::kDevTaskFolder) +
              L": " + Describe(hr));
    }
  }

  // The program may have left a process behind that still holds the output open for a moment.
  void RemoveRunFolder() {
    for (int attempt = 0; attempt < 15; ++attempt) {
      if (pdft::test::RemoveTree(runFolder_)) return;
      Sleep(200);
    }
    Message(L"could not delete the run folder " + runFolder_);
  }

  ComPtr<ITaskService> service_;
  ComPtr<ITaskFolder> folder_;
  std::wstring taskName_;
  std::wstring runFolder_;
  bool done_ = false;
};

class ComApartment {
 public:
  ComApartment() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;
  ~ComApartment() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  HRESULT hr() const { return hr_; }

 private:
  HRESULT hr_;
};

HRESULT DefineTask(ITaskService* service, const std::wstring& account, const std::wstring& self,
                   const std::wstring& runId, DWORD timeoutSeconds,
                   ComPtr<ITaskDefinition>* definition) {
  HRESULT hr = service->NewTask(0, definition->ReleaseAndGetAddressOf());
  if (FAILED(hr)) return hr;
  ITaskDefinition* task = definition->Get();

  ComPtr<IRegistrationInfo> info;
  if (FAILED(hr = task->get_RegistrationInfo(&info)) ||
      FAILED(hr = info->put_Description(Bstr(kTaskDescription).get()))) {
    return hr;
  }
  // The signed-in user, with that user's interactive token, without elevation.
  ComPtr<IPrincipal> principal;
  if (FAILED(hr = task->get_Principal(&principal)) ||
      FAILED(hr = principal->put_UserId(Bstr(account).get())) ||
      FAILED(hr = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
      FAILED(hr = principal->put_RunLevel(TASK_RUNLEVEL_LUA))) {
    return hr;
  }
  // Task Scheduler ends the task at its time limit even if this process is gone; a little later
  // than this process's own deadline (kTaskLimitMarginSeconds).
  ComPtr<ITaskSettings> settings;
  const std::wstring limit =
      L"PT" + std::to_wstring(timeoutSeconds + kTaskLimitMarginSeconds) + L"S";
  if (FAILED(hr = task->get_Settings(&settings)) ||
      FAILED(hr = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
      FAILED(hr = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
      FAILED(hr = settings->put_ExecutionTimeLimit(Bstr(limit).get())) ||
      FAILED(hr = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
      FAILED(hr = settings->put_Priority(kNormalTaskPriority)) ||
      FAILED(hr = settings->put_StartWhenAvailable(VARIANT_FALSE)) ||
      FAILED(hr = settings->put_AllowDemandStart(VARIANT_TRUE))) {
    return hr;
  }
  // The only action: this executable, as the child. The path is quoted (it may contain spaces);
  // the caller checked that it has no "%", which Task Scheduler would expand.
  ComPtr<IActionCollection> actions;
  ComPtr<IAction> action;
  ComPtr<IExecAction> exec;
  if (FAILED(hr = task->get_Actions(&actions)) ||
      FAILED(hr = actions->Create(TASK_ACTION_EXEC, &action)) || FAILED(hr = action.As(&exec)) ||
      FAILED(hr = exec->put_Path(Bstr(L"\"" + self + L"\"").get())) ||
      FAILED(hr = exec->put_Arguments(Bstr(L"--child " + runId).get()))) {
    return hr;
  }
  return S_OK;
}

// The dev task folder, made if it is missing.
HRESULT OpenTaskFolder(ITaskService* service, ComPtr<ITaskFolder>* folder) {
  const Bstr path(pdft::kDevTaskFolder);
  if (SUCCEEDED(service->GetFolder(path.get(), folder->ReleaseAndGetAddressOf()))) return S_OK;
  ComPtr<ITaskFolder> root;
  HRESULT hr = service->GetFolder(Bstr(L"\\").get(), &root);
  if (FAILED(hr)) return hr;
  hr = root->CreateFolder(path.get(), Empty(), folder->ReleaseAndGetAddressOf());
  if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {  // another run made it just now
    hr = service->GetFolder(path.get(), folder->ReleaseAndGetAddressOf());
  }
  return hr;
}

HRESULT RegisterTask(ITaskService* service, ITaskDefinition* definition, const std::wstring& name,
                     ComPtr<ITaskFolder>* folder, ComPtr<IRegisteredTask>* task) {
  for (int attempt = 0;; ++attempt) {
    HRESULT hr = OpenTaskFolder(service, folder);
    if (SUCCEEDED(hr)) {
      hr = (*folder)->RegisterTaskDefinition(Bstr(name).get(), definition, TASK_CREATE, Empty(),
                                             Empty(), TASK_LOGON_INTERACTIVE_TOKEN, Empty(),
                                             task->ReleaseAndGetAddressOf());
    }
    // Another run may have deleted the (then empty) folder in between: make it again, once.
    if (SUCCEEDED(hr) || attempt > 0 ||
        (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
         hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))) {
      return hr;
    }
  }
}

void StopTask(IRegisteredTask* task) {
  task->Stop(0);
  for (int i = 0; i < 50; ++i) {
    TASK_STATE state = TASK_STATE_UNKNOWN;
    if (FAILED(task->get_State(&state)) ||
        (state != TASK_STATE_RUNNING && state != TASK_STATE_QUEUED)) {
      return;
    }
    Sleep(kPollMs);
  }
}

struct Waited {
  HRESULT hr = S_OK;
  bool timedOut = false;
  bool interrupted = false;
  bool sawActive = false;  // the task was seen queued or running at least once
  ULONGLONG elapsedMs = 0;
};

// Waits until the started task is neither queued nor running; stops it at the time limit or on
// Ctrl+C. Only a task that was seen queued or running counts as timed out at the limit: one that
// never got that far is reported from its last result instead ("did not start").
Waited WaitForTask(IRegisteredTask* task, DWORD timeoutSeconds) {
  Waited waited;
  const ULONGLONG start = GetTickCount64();
  const ULONGLONG limit = static_cast<ULONGLONG>(timeoutSeconds) * 1000;
  for (;;) {
    TASK_STATE state = TASK_STATE_UNKNOWN;
    waited.hr = task->get_State(&state);
    if (FAILED(waited.hr)) break;
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (state == TASK_STATE_QUEUED || state == TASK_STATE_RUNNING) {
      waited.sawActive = true;
    } else {
      if (waited.sawActive || state == TASK_STATE_DISABLED) break;
      // It may have run and ended between two looks.
      LONG result = SCHED_S_TASK_HAS_NOT_RUN;
      if (SUCCEEDED(task->get_LastTaskResult(&result)) && result != SCHED_S_TASK_HAS_NOT_RUN) {
        break;
      }
      if (elapsed >= kStartGraceMs) break;  // it never started
    }
    if (elapsed >= limit) {
      // Stopped either way, so a task that was about to start cannot start after this.
      waited.timedOut = waited.sawActive;
      StopTask(task);
      break;
    }
    if (WaitForSingleObject(g_cancel, kPollMs) == WAIT_OBJECT_0) {
      waited.interrupted = true;
      StopTask(task);
      break;
    }
  }
  waited.elapsedMs = GetTickCount64() - start;
  return waited;
}

std::optional<DWORD> ReadResultRecord(const std::wstring& path) {
  UniqueHandle file = OpenRunFile(path);
  std::vector<BYTE> bytes;
  DWORD code = 0;
  if (!file || !ReadAll(file.get(), 64, &bytes) ||
      !outside::ParseResultRecord(
          std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &code)) {
    return std::nullopt;
  }
  return code;
}

// The program's output as it wrote it; to a console as text (read as UTF-8).
void PrintOutput(const std::wstring& path) {
  UniqueHandle file = OpenRunFile(path);
  if (!file) return;
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!out || out == INVALID_HANDLE_VALUE) return;
  if (IsConsole(out)) {
    std::vector<BYTE> bytes;
    if (!ReadAll(file.get(), kMaxConsoleOutput, &bytes)) {
      Message(L"the output is too large for the console; redirect it to a file");
      return;
    }
    WriteText(out,
              Wide(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    return;
  }
  std::vector<BYTE> buffer(1 << 16);
  DWORD read = 0;
  while (ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
         read > 0) {
    if (!WriteAll(out, buffer.data(), read)) return;
  }
}

int RunCaller(const outside::Options& options) {
  SetConsoleCtrlHandler(OnConsoleControl, TRUE);

  // The program, resolved against the current folder.
  std::wstring program;
  HRESULT hr = FullPath(options.program, &program);
  if (FAILED(hr)) {
    Message(L"cannot resolve " + options.program + L": " + Describe(hr));
    return outside::kExitFailed;
  }
  if (!outside::HasExeExtension(program)) {
    Message(L"can only run .exe programs (a script would need a shell): " + program);
    return outside::kExitFailed;
  }
  const DWORD attributes = GetFileAttributesW(program.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    Message(L"no such program: " + program);
    return outside::kExitFailed;
  }
  outside::Request request;
  request.program = program;
  request.arguments = options.arguments;
  std::wstring commandLine;
  std::vector<BYTE> requestBytes;
  if (FAILED(hr = CurrentFolder(&request.currentDirectory)) ||
      FAILED(hr = outside::BuildCommandLine(program, request.arguments, &commandLine)) ||
      FAILED(hr = outside::EncodeRequest(request, &requestBytes))) {
    Message(hr == E_BOUNDS ? L"the command line would be longer than Windows allows"
                           : L"cannot prepare the request: " + Describe(hr));
    return outside::kExitFailed;
  }
  std::wstring self;
  if (FAILED(hr = OwnPath(&self))) {
    Message(L"cannot find this program's own path: " + Describe(hr));
    return outside::kExitFailed;
  }
  if (self.find_first_of(L"%\"") != std::wstring::npos) {
    Message(L"this program's path contains % or a quote, which Task Scheduler would change: " +
            self);
    return outside::kExitFailed;
  }
  std::vector<BYTE> user;
  std::wstring sid;
  std::wstring account;
  std::wstring temp;
  GUID guid = {};
  if (FAILED(hr = UserSid(&user)) || FAILED(hr = SidString(user.data(), &sid)) ||
      FAILED(hr = AccountName(user.data(), &account)) || FAILED(hr = TempFolder(&temp)) ||
      FAILED(hr = CoCreateGuid(&guid))) {
    Message(L"cannot find the user, the temporary folder or a new GUID: " + Describe(hr));
    return outside::kExitFailed;
  }
  const std::wstring runId = outside::RunIdFromGuid(guid);
  const std::wstring runFolder = temp + pdft::kDevRunPrefix + runId;
  const std::wstring taskName = outside::kTaskNamePrefix + runId;

  // Declared first, so it ends last: the clean-up below still uses COM.
  ComApartment com;
  Cleanup cleanup;

  UniqueLocal folderDescriptor;
  UniqueLocal fileDescriptor;
  if (FAILED(hr = PrivateDescriptor(sid, true, &folderDescriptor)) ||
      FAILED(hr = PrivateDescriptor(sid, false, &fileDescriptor))) {
    Message(L"cannot build the run folder's security descriptor: " + Describe(hr));
    return outside::kExitFailed;
  }
  SECURITY_ATTRIBUTES folderAttributes = {sizeof(folderAttributes), folderDescriptor.get(), FALSE};
  if (!CreateDirectoryW(runFolder.c_str(), &folderAttributes)) {
    Message(L"cannot create " + runFolder + L": " + Describe(LastError()));
    return outside::kExitFailed;
  }
  cleanup.SetRunFolder(runFolder);
  hr = WriteNewFile(runFolder + L"\\" + outside::kRequestFile, requestBytes, fileDescriptor.get());
  if (FAILED(hr)) {
    Message(L"cannot write the request into " + runFolder + L": " + Describe(hr));
    return outside::kExitFailed;
  }

  if (FAILED(com.hr())) {
    Message(L"cannot start COM: " + Describe(com.hr()));
    return outside::kExitFailed;
  }
  ComPtr<ITaskService> service;
  if (FAILED(hr = CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&service))) ||
      FAILED(hr = service->Connect(Empty(), Empty(), Empty(), Empty()))) {
    Message(L"cannot connect to Task Scheduler: " + Describe(hr));
    return outside::kExitFailed;
  }
  ComPtr<ITaskDefinition> definition;
  hr = DefineTask(service.Get(), account, self, runId, options.timeoutSeconds, &definition);
  if (FAILED(hr)) {
    Message(L"cannot define the task: " + Describe(hr));
    return outside::kExitFailed;
  }
  ComPtr<ITaskFolder> taskFolder;
  ComPtr<IRegisteredTask> task;
  hr = RegisterTask(service.Get(), definition.Get(), taskName, &taskFolder, &task);
  if (FAILED(hr)) {
    // The folder may have been made just now for this run: the clean-up removes it if empty.
    if (taskFolder) cleanup.SetTask(service.Get(), taskFolder.Get(), std::wstring());
    Message(L"cannot register the task " + std::wstring(pdft::kDevTaskFolder) + L"\\" + taskName +
            L": " + Describe(hr));
    return outside::kExitFailed;
  }
  cleanup.SetTask(service.Get(), taskFolder.Get(), taskName);
  if (Cancelled()) {
    cleanup.Run();
    Message(L"interrupted before the program started");
    return outside::kExitInterrupted;
  }

  ComPtr<IRunningTask> running;
  hr = task->Run(Empty(), &running);
  if (FAILED(hr)) {
    Message(L"cannot start the task: " + Describe(hr));
    return outside::kExitFailed;
  }
  running.Reset();
  const Waited waited = WaitForTask(task.Get(), options.timeoutSeconds);
  if (FAILED(waited.hr)) {
    Message(L"cannot follow the task: " + Describe(waited.hr));
    StopTask(task.Get());
    return outside::kExitFailed;
  }

  outside::Outcome outcome;
  outcome.timeoutSeconds = options.timeoutSeconds;
  outcome.interrupted = waited.interrupted;
  outcome.childExitCode = ReadResultRecord(runFolder + L"\\" + outside::kResultFile);
  // A task that ran but left no record once the time was up was ended at the limit too.
  outcome.timedOut = waited.timedOut ||
                     (!outcome.childExitCode && waited.sawActive &&
                      waited.elapsedMs >= static_cast<ULONGLONG>(options.timeoutSeconds) * 1000);
  // A fresh look at the task, for the result of the run that just ended.
  ComPtr<IRegisteredTask> ended;
  LONG lastResult = SCHED_S_TASK_HAS_NOT_RUN;
  if (FAILED(taskFolder->GetTask(Bstr(taskName).get(), &ended)) ||
      FAILED(ended->get_LastTaskResult(&lastResult))) {
    task->get_LastTaskResult(&lastResult);
  }
  outcome.lastTaskResult = lastResult;

  PrintOutput(runFolder + L"\\" + outside::kOutputFile);
  ended.Reset();
  task.Reset();
  taskFolder.Reset();
  cleanup.Run();
  const outside::Verdict verdict = outside::Translate(outcome);
  if (!verdict.message.empty()) Message(verdict.message);
  return verdict.exitCode;
}

int Main(int argc, wchar_t** argv) {
  // DLLs that this process loads by name come from System32 only. Nothing that the started
  // program would inherit is changed.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
  HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

  std::vector<std::wstring_view> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  outside::Options options;
  std::wstring error;
  if (!outside::ParseOptions(args, &options, &error)) {
    Message(error);
    Message(L"run pdft-run-outside --help for usage");
    return outside::kExitFailed;
  }
  switch (options.mode) {
    case outside::Mode::kHelp:
      PrintUsage(GetStdHandle(STD_OUTPUT_HANDLE));
      return 0;
    case outside::Mode::kChild:
      return RunChild(options.runId);
    case outside::Mode::kRun:
      break;
  }
  g_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_cleanedUp = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_cancel || !g_cleanedUp) {
    Message(L"cannot create an event: " + Describe(LastError()));
    return outside::kExitFailed;
  }
  return RunCaller(options);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    return Main(argc, argv);
  } catch (...) {
    WriteRawError("pdft-run-outside: unexpected failure (out of memory?)\r\n");
    return outside::kExitFailed;
  }
}
