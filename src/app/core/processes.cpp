#include "processes.h"

#include <tlhelp32.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <utility>

#include "delay_load.h"
#include "strings.h"

namespace pdft {
namespace {

// How long TerminateOwnSessionHolder waits for an ended process to go.
constexpr DWORD kTerminateWaitMs = 5000;

// The exit code an ended holder gets.
constexpr UINT kTerminatedExitCode = 1;

// Restart Manager may report more processes on a second call than it counted on the first.
constexpr int kListAttempts = 5;

// Image paths are at most this long (the longest path Windows handles).
constexpr DWORD kMaxImageChars = 32768;

HRESULT LastErrorOr(HRESULT fallback) noexcept {
  const DWORD error = GetLastError();
  return error == ERROR_SUCCESS ? fallback : HRESULT_FROM_WIN32(error);
}

HRESULT ProcessStart(HANDLE process, FILETIME* start) noexcept {
  FILETIME unused[3] = {};
  RETURN_IF_WIN32_BOOL_FALSE(GetProcessTimes(process, start, &unused[0], &unused[1], &unused[2]));
  return S_OK;
}

HRESULT ProcessImage(HANDLE process, std::wstring* image) noexcept try {
  image->clear();
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    DWORD length = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) {
      buffer.resize(length);
      *image = std::move(buffer);
      return S_OK;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= kMaxImageChars) {
      return HRESULT_FROM_WIN32(error);
    }
    buffer.resize(buffer.size() * 2);
  }
}
CATCH_RETURN();

// Whether |process| runs as the same user as this process.
bool SameUser(HANDLE process) noexcept {
  wil::unique_handle theirs;
  wil::unique_handle ours;
  if (!OpenProcessToken(process, TOKEN_QUERY, theirs.put()) ||
      !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, ours.put())) {
    return false;
  }
  alignas(TOKEN_USER) BYTE theirUser[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  alignas(TOKEN_USER) BYTE ourUser[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  if (!GetTokenInformation(theirs.get(), TokenUser, theirUser, sizeof(theirUser), &size) ||
      !GetTokenInformation(ours.get(), TokenUser, ourUser, sizeof(ourUser), &size)) {
    return false;
  }
  return EqualSid(reinterpret_cast<TOKEN_USER*>(theirUser)->User.Sid,
                  reinterpret_cast<TOKEN_USER*>(ourUser)->User.Sid) != FALSE;
}

bool SameImage(std::wstring_view a, std::wstring_view b) {
  const std::filesystem::path left = std::filesystem::path(a).lexically_normal();
  const std::filesystem::path right = std::filesystem::path(b).lexically_normal();
  return left.is_absolute() && IEquals(left.native(), right.native());
}

}  // namespace

HRESULT FindHolders(std::span<const std::filesystem::path> files,
                    std::vector<Holder>* holders) noexcept try {
  holders->clear();
  if (files.empty()) return S_OK;
  RETURN_IF_FAILED(EnsureDelayLoaded("rstrtmgr.dll"));

  DWORD session = 0;
  wchar_t key[CCH_RM_SESSION_KEY + 1] = {};
  RETURN_IF_WIN32_ERROR(RmStartSession(&session, 0, key));
  const auto end = wil::scope_exit([session]() noexcept { RmEndSession(session); });

  std::vector<LPCWSTR> names;
  names.reserve(files.size());
  for (const std::filesystem::path& file : files) names.push_back(file.c_str());
  RETURN_IF_WIN32_ERROR(RmRegisterResources(session, static_cast<UINT>(names.size()), names.data(),
                                            0, nullptr, 0, nullptr));

  std::vector<RM_PROCESS_INFO> found;
  DWORD status = ERROR_MORE_DATA;
  for (int attempt = 0; attempt < kListAttempts && status == ERROR_MORE_DATA; ++attempt) {
    UINT needed = 0;
    UINT count = static_cast<UINT>(found.size());
    DWORD reasons = RmRebootReasonNone;
    status = RmGetList(session, &needed, &count, found.empty() ? nullptr : found.data(), &reasons);
    if (status == ERROR_MORE_DATA) {
      found.resize(needed);
    } else if (status == ERROR_SUCCESS) {
      found.resize(count);
    }
  }
  RETURN_IF_WIN32_ERROR(status);

  for (const RM_PROCESS_INFO& info : found) {
    Holder holder;
    holder.pid = info.Process.dwProcessId;
    holder.start = info.Process.ProcessStartTime;
    holder.session = info.TSSessionId;
    holder.type = info.ApplicationType;
    holder.app = info.strAppName;
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, holder.pid));
    if (process) {
      FILETIME start = {};
      // Gone and its id reused since Restart Manager looked: not a holder any more.
      if (FAILED(ProcessStart(process.get(), &start)) ||
          CompareFileTime(&start, &holder.start) != 0) {
        continue;
      }
      (void)ProcessImage(process.get(), &holder.image);  // empty when it cannot be read
    } else if (GetLastError() == ERROR_INVALID_PARAMETER) {
      continue;  // gone
    }
    holders->push_back(std::move(holder));
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT SurrogateImagePath(std::wstring* image) noexcept try {
  image->clear();
  wchar_t system[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
  RETURN_LAST_ERROR_IF(length == 0);
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length >= ARRAYSIZE(system));
  *image = (std::filesystem::path(system) / L"dllhost.exe").native();
  return S_OK;
}
CATCH_RETURN();

HolderRefusal JudgeHolder(const Holder& found, const HolderFacts& now,
                          std::span<const std::wstring_view> allowedImages) noexcept {
  if (found.pid == now.ownPid) return HolderRefusal::kSelf;
  if (CompareFileTime(&found.start, &now.start) != 0) return HolderRefusal::kReplaced;
  if (found.session != now.ownSession || now.session != now.ownSession) {
    return HolderRefusal::kOtherSession;
  }
  if (!now.sameUser) return HolderRefusal::kOtherUser;
  if (now.image.empty()) return HolderRefusal::kNotAllowed;
  try {
    for (const std::wstring_view allowed : allowedImages) {
      if (SameImage(now.image, allowed)) return HolderRefusal::kNone;
    }
  } catch (...) {
  }
  return HolderRefusal::kNotAllowed;
}

HRESULT TerminateOwnSessionHolder(const Holder& holder,
                                  std::span<const std::wstring_view> allowedImages) noexcept try {
  wil::unique_handle process(OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, holder.pid));
  if (!process) {
    const DWORD error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) return S_FALSE;  // gone
    // Not ours to end (another account's, or elevated): a refusal like any other.
    if (error == ERROR_ACCESS_DENIED) return E_ACCESSDENIED;
    return HRESULT_FROM_WIN32(error);
  }
  if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) return S_FALSE;  // ended already

  HolderFacts facts;
  facts.ownPid = GetCurrentProcessId();
  RETURN_IF_WIN32_BOOL_FALSE(ProcessIdToSessionId(facts.ownPid, &facts.ownSession));
  if (!ProcessIdToSessionId(holder.pid, &facts.session)) return E_ACCESSDENIED;
  RETURN_IF_FAILED(ProcessStart(process.get(), &facts.start));
  if (FAILED(ProcessImage(process.get(), &facts.image))) facts.image.clear();
  facts.sameUser = SameUser(process.get());

  switch (JudgeHolder(holder, facts, allowedImages)) {
    case HolderRefusal::kNone:
      break;
    case HolderRefusal::kReplaced:
      return S_FALSE;
    default:
      return E_ACCESSDENIED;
  }
  if (!TerminateProcess(process.get(), kTerminatedExitCode)) {
    // It may have ended on its own meanwhile.
    if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) return S_FALSE;
    return LastErrorOr(E_FAIL);
  }
  if (WaitForSingleObject(process.get(), kTerminateWaitMs) != WAIT_OBJECT_0) {
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT GetParentProcessId(DWORD* parent) noexcept try {
  *parent = 0;
  HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  RETURN_LAST_ERROR_IF(rawSnapshot == INVALID_HANDLE_VALUE);
  wil::unique_handle snapshot(rawSnapshot);
  const DWORD self = GetCurrentProcessId();
  DWORD candidate = 0;
  PROCESSENTRY32W entry = {sizeof(entry)};
  for (BOOL more = Process32FirstW(snapshot.get(), &entry); more;
       more = Process32NextW(snapshot.get(), &entry)) {
    if (entry.th32ProcessID == self) {
      candidate = entry.th32ParentProcessID;
      break;
    }
  }
  if (candidate == 0 || candidate == self) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate));
  if (!process) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  FILETIME created = {};
  FILETIME ownCreated = {};
  RETURN_IF_FAILED(ProcessStart(process.get(), &created));
  RETURN_IF_FAILED(ProcessStart(GetCurrentProcess(), &ownCreated));
  // Created after this process: the real parent ended and its id went to someone else.
  if (CompareFileTime(&created, &ownCreated) > 0) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  *parent = candidate;
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
