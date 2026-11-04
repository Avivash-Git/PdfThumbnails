#include "processes.h"

// userenv.h before (another inclusion of) wil/resource.h enables WIL's environment block wrapper.
#include <tlhelp32.h>
#include <userenv.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "delay_load.h"
#include "strings.h"

namespace pdft {
namespace {

// How long TerminateOwnSessionHolder, and a ProgramStart that ends what it started, wait for an
// ended process to go.
constexpr DWORD kTerminateWaitMs = 5000;

// The exit code an ended holder gets.
constexpr UINT kTerminatedExitCode = 1;

// The exit code a process ProgramStart started suspended and ended gets.
constexpr UINT kTrialExitCode = ERROR_CANCELLED;

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

// A Windows folder from |get| (GetSystemDirectoryW and its kind).
HRESULT WindowsFolder(UINT(WINAPI* get)(LPWSTR, UINT), std::filesystem::path* folder) noexcept try {
  folder->clear();
  wchar_t buffer[MAX_PATH] = {};
  const UINT length = get(buffer, ARRAYSIZE(buffer));
  RETURN_LAST_ERROR_IF(length == 0);
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length >= ARRAYSIZE(buffer));
  *folder = buffer;
  return S_OK;
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

// Creates |image| suspended with |commandLine|, |environment| and |folder|, nothing inherited,
// asking to leave this process's job (|job|) as ChooseBreakaway says. *refused when that job did
// not let it leave (JobRefusedStart).
HRESULT CreateSuspended(const std::wstring& image, std::wstring* commandLine, void* environment,
                        const std::filesystem::path& folder, const JobFacts& job,
                        DWORD startupFlags, wil::unique_process_information* process,
                        bool* refused) noexcept {
  *refused = false;
  STARTUPINFOW startup = {sizeof(startup)};
  startup.dwFlags = startupFlags;
  const DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE |
                      (ChooseBreakaway(job) == BreakawayMode::kAsk ? CREATE_BREAKAWAY_FROM_JOB : 0);
  if (!CreateProcessW(image.c_str(), commandLine->data(), nullptr, nullptr, FALSE, flags,
                      environment, folder.c_str(), &startup, process)) {
    const DWORD error = GetLastError();
    *refused = JobRefusedStart(job, error);
    return HRESULT_FROM_WIN32(error);
  }
  return S_OK;
}

// Ends |process|, which ProgramStart created suspended, and waits for it to go.
HRESULT EndAndWait(HANDLE process) noexcept {
  if (!TerminateProcess(process, kTrialExitCode)) return LastErrorOr(E_FAIL);
  switch (WaitForSingleObject(process, kTerminateWaitMs)) {
    case WAIT_OBJECT_0:
      return S_OK;
    case WAIT_FAILED:
      return LastErrorOr(E_FAIL);
    default:
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
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
  std::filesystem::path system;
  RETURN_IF_FAILED(WindowsFolder(GetSystemDirectoryW, &system));
  *image = (system / L"dllhost.exe").native();
  return S_OK;
}
CATCH_RETURN();

HRESULT GetRebuildImages(RebuildImages* images) noexcept try {
  *images = {};
  RebuildImages found;
  std::filesystem::path folder;
  // The system's own Windows folder, also for a user whose GetWindowsDirectoryW is private
  // (Terminal Services).
  RETURN_IF_FAILED(WindowsFolder(GetSystemWindowsDirectoryW, &folder));
  found.explorer = (folder / L"explorer.exe").native();
  RETURN_IF_FAILED(SurrogateImagePath(&found.surrogate));
  wchar_t wow64[MAX_PATH] = {};
  const UINT length = GetSystemWow64DirectoryW(wow64, ARRAYSIZE(wow64));
  if (length == 0) {
    // No WOW64 on this Windows: no 32-bit thumbnail process either.
    RETURN_LAST_ERROR_IF(GetLastError() != ERROR_CALL_NOT_IMPLEMENTED);
  } else {
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length >= ARRAYSIZE(wow64));
    found.surrogateWow64 = (std::filesystem::path(wow64) / L"dllhost.exe").native();
  }
  *images = std::move(found);
  return S_OK;
}
CATCH_RETURN();

bool OwnProcessStartedSince(std::wstring_view image, const FILETIME& since) noexcept {
  HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (rawSnapshot == INVALID_HANDLE_VALUE) return false;
  wil::unique_handle snapshot(rawSnapshot);
  const DWORD self = GetCurrentProcessId();
  DWORD ownSession = 0;
  if (!ProcessIdToSessionId(self, &ownSession)) return false;
  const std::wstring_view leaf = image.substr(image.find_last_of(L"\\/") + 1);
  PROCESSENTRY32W entry = {sizeof(entry)};
  for (BOOL more = Process32FirstW(snapshot.get(), &entry); more;
       more = Process32NextW(snapshot.get(), &entry)) {
    DWORD session = 0;
    if (entry.th32ProcessID == self || !IEquals(entry.szExeFile, leaf) ||
        !ProcessIdToSessionId(entry.th32ProcessID, &session) || session != ownSession) {
      continue;
    }
    wil::unique_handle process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
    FILETIME start = {};
    std::wstring found;
    if (!process || FAILED(ProcessStart(process.get(), &start)) ||
        CompareFileTime(&start, &since) <= 0 || FAILED(ProcessImage(process.get(), &found)) ||
        !SameImagePath(found, image) || !SameUser(process.get())) {
      continue;
    }
    return true;
  }
  return false;
}

HRESULT ReadOwnJob(JobFacts* facts) noexcept {
  *facts = {};
  BOOL inJob = FALSE;
  RETURN_IF_WIN32_BOOL_FALSE(IsProcessInJob(GetCurrentProcess(), nullptr, &inJob));
  facts->inJob = inJob != FALSE;
  if (!facts->inJob) return S_OK;
  // Without a handle: this process's immediate job.
  JOBOBJECT_BASIC_LIMIT_INFORMATION limits = {};
  if (QueryInformationJobObject(nullptr, JobObjectBasicLimitInformation, &limits, sizeof(limits),
                                nullptr)) {
    facts->limitFlags = limits.LimitFlags;
  }
  return S_OK;
}

BreakawayMode ChooseBreakaway(const JobFacts& facts) noexcept {
  if (!facts.inJob) return BreakawayMode::kNotInJob;
  // Limits that cannot be read: ask, as a start always did; a job that refuses says so.
  if (!facts.limitFlags) return BreakawayMode::kAsk;
  if (*facts.limitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) return BreakawayMode::kAsk;
  if (*facts.limitFlags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) return BreakawayMode::kSilent;
  return BreakawayMode::kForbidden;
}

bool JobRefusedStart(const JobFacts& facts, DWORD error) noexcept {
  // Only a start that asks to leave (kAsk) can be refused, and with known limits it is asked only
  // where they allow it.
  return ChooseBreakaway(facts) == BreakawayMode::kAsk && !facts.limitFlags &&
         error == ERROR_ACCESS_DENIED;
}

bool ChildEndsWithJob(const JobFacts& facts) noexcept {
  if (!facts.inJob) return false;
  if (!facts.limitFlags) return true;
  return (*facts.limitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0 &&
         (*facts.limitFlags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) == 0;
}

std::wstring JobFactsToken(const JobFacts& facts) {
  if (!facts.inJob) return L"none";
  if (!facts.limitFlags) return L"unread";
  constexpr DWORD kShown = JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
                           JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  return HResultText(static_cast<HRESULT>(*facts.limitFlags & kShown));
}

ProgramStart::~ProgramStart() = default;

HRESULT ProgramStart::Prepare(std::wstring image, std::wstring arguments,
                              std::unique_ptr<ProgramStart>* start) noexcept try {
  start->reset();
  std::unique_ptr<ProgramStart> made(new ProgramStart());
  RETURN_IF_FAILED(WindowsFolder(GetSystemDirectoryW, &made->system32_));
  RETURN_IF_FAILED(EnsureDelayLoaded("userenv.dll"));
  wil::unique_handle token;
  RETURN_IF_WIN32_BOOL_FALSE(OpenProcessToken(
      GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, token.put()));
  // The user's variables from the profile only: nothing of this process's environment.
  RETURN_IF_WIN32_BOOL_FALSE(CreateEnvironmentBlock(made->environment_.put(), token.get(), FALSE));
  RETURN_IF_FAILED(ReadOwnJob(&made->job_));
  made->mode_ = ChooseBreakaway(made->job_);
  // One command line each: CreateProcessW may write to the one it is given.
  std::wstring commandLine = L"\"" + image + L"\"";
  if (!arguments.empty()) commandLine += L" " + arguments;
  made->trialCommandLine_ = commandLine;
  made->startCommandLine_ = std::move(commandLine);
  made->image_ = std::move(image);
  *start = std::move(made);
  return S_OK;
}
CATCH_RETURN();

HRESULT ProgramStart::Trial(JobEscape* escape) noexcept {
  *escape = JobEscape::kForbidden;
  RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, tried_);
  tried_ = true;
  if (mode_ == BreakawayMode::kForbidden) return S_OK;  // nothing is started
  wil::unique_process_information trial;
  bool refused = false;
  const HRESULT created = CreateSuspended(image_, &trialCommandLine_, environment_.get(), system32_,
                                          job_, STARTF_FORCEOFFFEEDBACK, &trial, &refused);
  if (refused) return S_OK;  // the job let nothing leave it after all
  RETURN_IF_FAILED(created);
  BOOL inJob = FALSE;
  const HRESULT jobHr =
      IsProcessInJob(trial.hProcess, nullptr, &inJob) ? S_OK : LastErrorOr(E_FAIL);
  // A trial process that may still exist is never a pass, whatever its jobs.
  RETURN_IF_FAILED(EndAndWait(trial.hProcess));
  RETURN_IF_FAILED(jobHr);
  *escape = inJob ? JobEscape::kStays : JobEscape::kLeaves;
  return S_OK;
}

HRESULT ProgramStart::Start(bool* jobForbids) noexcept {
  *jobForbids = false;
  RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, started_);
  started_ = true;
  if (mode_ == BreakawayMode::kForbidden) {
    *jobForbids = true;
    return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  }
  wil::unique_process_information started;
  bool refused = false;
  const HRESULT created = CreateSuspended(image_, &startCommandLine_, environment_.get(), system32_,
                                          job_, 0, &started, &refused);
  if (FAILED(created)) {
    *jobForbids = refused;
    return created;
  }
  BOOL inJob = FALSE;
  const HRESULT jobHr =
      IsProcessInJob(started.hProcess, nullptr, &inJob) ? S_OK : LastErrorOr(E_FAIL);
  if (FAILED(jobHr) || inJob) {
    // Never left running inside a job: the job's end would take it along.
    *jobForbids = SUCCEEDED(jobHr);
    RETURN_IF_FAILED(EndAndWait(started.hProcess));
    return FAILED(jobHr) ? jobHr : HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  }
  if (ResumeThread(started.hThread) == static_cast<DWORD>(-1)) {
    const HRESULT resumeHr = LastErrorOr(E_FAIL);
    (void)EndAndWait(started.hProcess);
    return resumeHr;
  }
  return S_OK;
}

bool SameImagePath(std::wstring_view a, std::wstring_view b) noexcept {
  try {
    const std::filesystem::path left = std::filesystem::path(a).lexically_normal();
    const std::filesystem::path right = std::filesystem::path(b).lexically_normal();
    return left.is_absolute() && IEquals(left.native(), right.native());
  } catch (...) {
    return false;  // out of memory: "not the same" is the safe answer
  }
}

HolderRefusal JudgeHolder(const Holder& found, const HolderFacts& now,
                          std::span<const std::wstring_view> allowedImages) noexcept {
  if (found.pid == now.ownPid) return HolderRefusal::kSelf;
  if (CompareFileTime(&found.start, &now.start) != 0) return HolderRefusal::kReplaced;
  if (found.session != now.ownSession || now.session != now.ownSession) {
    return HolderRefusal::kOtherSession;
  }
  if (!now.sameUser) return HolderRefusal::kOtherUser;
  if (now.image.empty()) return HolderRefusal::kNotAllowed;
  for (const std::wstring_view allowed : allowedImages) {
    if (SameImagePath(now.image, allowed)) return HolderRefusal::kNone;
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

HRESULT GetParentProcessId(DWORD* parent) noexcept {
  wil::unique_handle process;
  return OpenParentProcess(parent, std::addressof(process));
}

HRESULT OpenParentProcess(DWORD* parentId, wil::unique_handle* parent) noexcept try {
  *parentId = 0;
  parent->reset();
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
  *parentId = candidate;
  *parent = std::move(process);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
