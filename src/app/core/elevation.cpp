#include "elevation.h"

#include <sddl.h>
#include <shellapi.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <cwchar>
#include <utility>
#include <vector>

#include "context.h"
#include "failure_trace.h"
#include "file_ops.h"
#include "install.h"
#include "instance.h"
#include "legacy.h"
#include "log.h"
#include "migrate_v1.h"
#include "paths.h"
#include "processes.h"
#include "project_config.h"
#include "strings.h"
#include "uninstall.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"elevated";

constexpr std::wstring_view kElevatedOption = L"--elevated";

// The parent keeps at most this many bytes of report; the rest is dropped.
constexpr size_t kMaxReportBytes = 256 * 1024;

// Once the elevated process has ended, the rest of its lines are already in the pipe: this is
// how long Finish waits for the reader to get them.
constexpr DWORD kFinishWaitMs = 3000;

struct VerbName {
  ElevatedVerb verb;
  std::wstring_view name;
  bool devOnly;
};

constexpr VerbName kVerbNames[] = {
    {ElevatedVerb::kMachineInstall, L"machine-install", true},
    {ElevatedVerb::kMachineRepairClass, L"machine-repair-class", false},
    {ElevatedVerb::kMachineUninstall, L"machine-uninstall", false},
    {ElevatedVerb::kMachineMigrateV1, L"machine-migrate-v1", false},
};

HRESULT LastErrorOr(HRESULT fallback) noexcept {
  const DWORD error = GetLastError();
  return error == ERROR_SUCCESS ? fallback : HRESULT_FROM_WIN32(error);
}

bool IsControl(wchar_t c) noexcept { return c < L' ' || (c >= 0x7F && c <= 0x9F); }

// UTF-8 to UTF-16 with U+FFFD for malformed bytes (the pipe's bytes are not trusted to be
// well formed). May throw std::bad_alloc.
std::wstring DecodeLossy(std::string_view bytes) {
  if (bytes.empty()) return {};
  const int size = static_cast<int>(std::min<size_t>(bytes.size(), kMaxReportBytes));
  const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), size, nullptr, 0);
  if (length <= 0) return {};
  std::wstring text(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, bytes.data(), size, text.data(), length);
  return text;
}

// The facts CheckElevatedEntry judges, read from this process.
HRESULT GatherEntryFacts(ElevatedEntryFacts* facts) noexcept try {
  *facts = ElevatedEntryFacts{};
  facts->devInstall = kDevInstall;
  RETURN_IF_FAILED(GetTokenElevation(&facts->tokenElevated));

  // A walk that fails counts as packaged: the step is refused rather than run unchecked.
  std::optional<std::wstring> package;
  facts->packaged = FAILED(FindPackagedAncestor(&package)) || package.has_value();

  int count = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
  const wil::unique_hlocal argvMemory(argv);
  if (argv && count >= 1) {
    std::vector<std::wstring_view> args;
    for (int i = 1; i < count; ++i) args.emplace_back(argv[i]);
    facts->verb = ParseElevatedArguments(args, facts->devInstall);
  }

  std::filesystem::path root;
  std::filesystem::path own;
  std::filesystem::path canonicalRoot;
  std::filesystem::path canonicalOwn;
  if (SUCCEEDED(GetMachineRoot(&root))) {
    facts->reparsePoints = CheckNoReparsePoints(root, {});
    facts->runsFromMachineRoot =
        SUCCEEDED(GetOwnExe(&own)) && SUCCEEDED(Canonicalize(own, &canonicalOwn)) &&
        SUCCEEDED(Canonicalize(root, &canonicalRoot)) &&
        IEquals(canonicalOwn.parent_path().native(), canonicalRoot.native()) &&
        IEquals(canonicalOwn.filename().native(), kSettingsExe);
  } else {
    facts->reparsePoints = E_UNEXPECTED;
  }
  return S_OK;
}
CATCH_RETURN();

// One report line per folder of a removal: "<what> <folder name>".
void ReportRemoval(ElevatedReporter& report, const MachineRemoval& removal) {
  for (const std::wstring& name : removal.removed) report.Line(L"removed " + name);
  for (const std::wstring& name : removal.left) report.Line(L"in use, left " + name);
  for (const std::wstring& name : removal.failed) report.Line(L"failed " + name);
}

// machine-migrate-v1: 1.x's build folders for all users, except the one the machine-wide class
// still uses. In-use files go to a trash folder next to the install folder (same volume).
HRESULT RunMachineMigrateV1(ElevatedReporter& report, bool* restartPending) noexcept try {
  TraceStep step(L"legacy-root");
  std::filesystem::path legacyRoot;
  RETURN_IF_FAILED(PDFT_TRACE(GetLegacyMachineRoot(&legacyRoot)));
  RETURN_IF_FAILED(PDFT_TRACE(CheckNoReparsePoints(legacyRoot, {})));
  step.Set(L"remove-1x");
  MachineRemoval removal;
  const HRESULT hr =
      PDFT_TRACE(MigrateV1Machine(RegistryRoots{}, legacyRoot, legacyRoot.parent_path(), &removal));
  ReportRemoval(report, removal);
  if (removal.classInV1) {
    report.Line(L"the class for all users still points at a 1.x build; installing " +
                std::wstring(kProductName) + L" for all users repoints it");
  }
  *restartPending = removal.restartPending;
  RETURN_IF_FAILED(hr);
  // The failures it went on after are listed above (ReportRemoval).
  return PDFT_TRACE(removal.firstError);
}
CATCH_RETURN();

}  // namespace

HRESULT GetTokenElevation(bool* elevated) noexcept {
  *elevated = false;
  TOKEN_ELEVATION elevation = {};
  DWORD size = 0;
  // The pseudo handle of this process's token: nothing to open or close.
  RETURN_IF_WIN32_BOOL_FALSE(GetTokenInformation(GetCurrentProcessToken(), TokenElevation,
                                                 &elevation, sizeof(elevation), &size));
  *elevated = elevation.TokenIsElevated != 0;
  return S_OK;
}

HRESULT ReadTokenFacts(HANDLE token, TokenFacts* facts) noexcept try {
  *facts = TokenFacts{};
  TOKEN_ELEVATION elevation = {};
  DWORD size = 0;
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size));
  facts->elevated = elevation.TokenIsElevated != 0;
  RETURN_IF_WIN32_BOOL_FALSE(GetTokenInformation(token, TokenElevationType, &facts->elevationType,
                                                 sizeof(facts->elevationType), &size));

  alignas(TOKEN_MANDATORY_LABEL) BYTE label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &size));
  const PSID labelSid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
  const UCHAR count = *GetSidSubAuthorityCount(labelSid);
  facts->integrityRid = count ? *GetSidSubAuthority(labelSid, count - 1u) : 0;

  BYTE administrators[SECURITY_MAX_SID_SIZE];
  DWORD administratorsSize = sizeof(administrators);
  RETURN_IF_WIN32_BOOL_FALSE(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                                administrators, &administratorsSize));
  size = 0;
  GetTokenInformation(token, TokenGroups, nullptr, 0, &size);  // the size it needs
  RETURN_LAST_ERROR_IF(size == 0);
  std::vector<BYTE> buffer(size);
  RETURN_IF_WIN32_BOOL_FALSE(GetTokenInformation(token, TokenGroups, buffer.data(), size, &size));
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if (!EqualSid(groups->Groups[i].Sid, administrators)) continue;
    const DWORD attributes = groups->Groups[i].Attributes;
    facts->administrators = (attributes & SE_GROUP_USE_FOR_DENY_ONLY) ? TokenFacts::Group::kDenyOnly
                            : (attributes & SE_GROUP_ENABLED) ? TokenFacts::Group::kEnabled
                                                              : TokenFacts::Group::kDisabled;
    break;
  }
  return S_OK;
}
CATCH_RETURN();

std::wstring TokenFactsText(const TokenFacts& facts) {
  const wchar_t* type = L"default";
  if (facts.elevationType == TokenElevationTypeFull) type = L"full";
  if (facts.elevationType == TokenElevationTypeLimited) type = L"limited";
  const wchar_t* administrators = L"absent";
  switch (facts.administrators) {
    case TokenFacts::Group::kEnabled:
      administrators = L"enabled";
      break;
    case TokenFacts::Group::kDenyOnly:
      administrators = L"deny-only";
      break;
    case TokenFacts::Group::kDisabled:
      administrators = L"not enabled";
      break;
    case TokenFacts::Group::kAbsent:
      break;
  }
  wchar_t integrity[16] = {};
  swprintf_s(integrity, L"0x%04lX", static_cast<unsigned long>(facts.integrityRid));
  return std::wstring(L"token: elevated ") + (facts.elevated ? L"yes" : L"no") +
         L", elevation type " + std::to_wstring(static_cast<int>(facts.elevationType)) + L" (" +
         type + L"), integrity " + integrity + L", Administrators " + administrators;
}

std::wstring_view ElevatedVerbName(ElevatedVerb verb) noexcept {
  for (const VerbName& entry : kVerbNames) {
    if (entry.verb == verb) return entry.name;
  }
  return {};
}

std::optional<ElevatedVerb> ElevatedVerbFromName(std::wstring_view name, bool devInstall) noexcept {
  for (const VerbName& entry : kVerbNames) {
    if (entry.name == name && (!entry.devOnly || devInstall)) return entry.verb;
  }
  return std::nullopt;
}

ElevatedVerb ElevatedVerbFromStep(ElevatedStep step) noexcept {
  switch (step) {
    case ElevatedStep::kMachineInstall:
      return ElevatedVerb::kMachineInstall;
    case ElevatedStep::kMachineRepairClass:
      return ElevatedVerb::kMachineRepairClass;
    case ElevatedStep::kMachineUninstall:
      return ElevatedVerb::kMachineUninstall;
    case ElevatedStep::kMachineMigrateV1:
      break;
  }
  return ElevatedVerb::kMachineMigrateV1;
}

std::wstring ElevatedParameters(ElevatedVerb verb) {
  return std::wstring(kElevatedOption) + L" " + std::wstring(ElevatedVerbName(verb));
}

std::optional<ElevatedVerb> ParseElevatedArguments(std::span<const std::wstring_view> args,
                                                   bool devInstall) noexcept {
  if (args.size() != 2 || args[0] != kElevatedOption) return std::nullopt;
  return ElevatedVerbFromName(args[1], devInstall);
}

HRESULT CheckElevatedEntry(const ElevatedEntryFacts& facts) noexcept {
  if (!facts.tokenElevated) return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);
  if (facts.packaged) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED_IN_APPCONTAINER);
  if (!facts.verb) return E_INVALIDARG;
  const bool exempt = facts.devInstall && *facts.verb == ElevatedVerb::kMachineInstall;
  if (!facts.runsFromMachineRoot && !exempt) return E_ACCESSDENIED;
  if (FAILED(facts.reparsePoints)) return facts.reparsePoints;
  return S_OK;
}

int ElevatedMain() noexcept {
  ElevatedEntryFacts facts;
  HRESULT hr = GatherEntryFacts(&facts);
  if (SUCCEEDED(hr)) hr = CheckElevatedEntry(facts);
  // The report goes only to this process's own parent, so even a refusal may say why.
  ElevatedReporter report = ElevatedReporter::Connect();
  const std::wstring_view name =
      facts.verb ? ElevatedVerbName(*facts.verb) : std::wstring_view(L"(none)");
  try {
    // Once, first: what this process's token can do decides much of what the step can.
    TokenFacts token;
    const HRESULT read = ReadTokenFacts(GetCurrentProcessToken(), &token);
    const std::wstring tokenText =
        SUCCEEDED(read) ? TokenFactsText(token) : L"token: unreadable, hr=" + HResultText(read);
    if (FAILED(hr)) {
      report.Line(L"elevated " + std::wstring(name) + L" refused hr=" + HResultText(hr) + L" (" +
                  tokenText + L")");
      return static_cast<int>(hr);
    }
    report.Line(L"elevated " + std::wstring(name) + L" start (" + tokenText + L")");
  } catch (...) {
    return static_cast<int>(FAILED(hr) ? hr : E_OUTOFMEMORY);
  }
  bool restartPending = false;
  // Where the step's failure started: the functions on its way note it (failure_trace.h).
  FailureTrace trace;
  hr = RunElevatedStep(*facts.verb, report, &restartPending);
  try {
    if (FAILED(hr)) {
      std::wstring where = trace.failure() ? TracedFailureText(*trace.failure())
                                           : std::wstring(L"at a place not traced");
      // A caller may pass a failure on as another HRESULT; the end line says which.
      if (trace.failure() && trace.failure()->hr != hr) where += L" (end hr differs)";
      report.Line(L"elevated " + std::wstring(name) + L" failed " + where);
    }
    report.Line(L"elevated " + std::wstring(name) + L" end hr=" + HResultText(hr) +
                (restartPending ? L" restart-pending" : L""));
  } catch (...) {
  }
  if (FAILED(hr)) return static_cast<int>(hr);
  return restartPending ? 1 : 0;
}

HRESULT RunElevatedStep(ElevatedVerb verb, ElevatedReporter& report,
                        bool* restartPending) noexcept {
  *restartPending = false;
  switch (verb) {
    case ElevatedVerb::kMachineMigrateV1:
      return RunMachineMigrateV1(report, restartPending);
    case ElevatedVerb::kMachineInstall:
      // The parser and the entry checks keep it out of builds without the developer install.
      if (!kDevInstall) break;
      return RunMachineInstall(report, restartPending);
    case ElevatedVerb::kMachineRepairClass:
      return RunMachineRepairClass(report);
    case ElevatedVerb::kMachineUninstall:
      return RunMachineUninstall(report, restartPending);
  }
  return E_NOTIMPL;
}

// ---- reporting channel ----

std::wstring ElevationPipeName(DWORD parentProcessId) {
  return std::wstring(L"\\\\.\\pipe\\") + kObjectPrefix + L".Elevated." +
         std::to_wstring(parentProcessId);
}

ElevatedReporter ElevatedReporter::Connect() noexcept {
  ElevatedReporter reporter;
  DWORD parent = 0;
  if (FAILED(GetParentProcessId(&parent))) return reporter;
  std::wstring name;
  try {
    name = ElevationPipeName(parent);
  } catch (...) {
    return reporter;
  }
  // Anonymous: whoever serves the pipe cannot impersonate this (elevated) process.
  wil::unique_hfile pipe(CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
  if (!pipe) return reporter;
  ULONG server = 0;
  if (!GetNamedPipeServerProcessId(pipe.get(), &server) || server != parent) return reporter;
  reporter.pipe_ = std::move(pipe);
  return reporter;
}

void ElevatedReporter::Line(std::wstring_view text) noexcept {
  if (!pipe_) return;
  std::wstring clean(text.substr(0, kMaxReportLineChars));
  std::replace_if(clean.begin(), clean.end(), IsControl, L' ');
  std::string bytes = WideToUtf8Lossy(clean);
  bytes.push_back('\n');
  DWORD written = 0;
  if (!WriteFile(pipe_.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
      written != bytes.size()) {
    pipe_.reset();
  }
}

struct ElevationReportServer::State {
  wil::unique_hfile pipe;
  wil::unique_event_nothrow io;         // the pipe's overlapped operations
  wil::unique_event_nothrow childDone;  // the elevated process has ended
  wil::unique_event_nothrow stop;       // stop now
  OVERLAPPED overlapped = {};
  bool connected = false;
  DWORD clientProcessId = 0;
  std::string bytes;
};

namespace {

// Waits for the pending operation on |state|'s pipe, or for one of |others| (at most two). True
// with the byte count when the operation completed; false when it failed or another handle came
// first (the operation is then cancelled and finished).
bool AwaitPipe(ElevationReportServer::State& state, std::span<const HANDLE> others,
               DWORD* transferred) noexcept {
  HANDLE waits[3] = {state.io.get()};
  const size_t count = 1 + std::min<size_t>(others.size(), 2);
  std::copy_n(others.begin(), count - 1, waits + 1);
  const DWORD waited = WaitForMultipleObjects(static_cast<DWORD>(count), waits, FALSE, INFINITE);
  if (waited != WAIT_OBJECT_0) {
    CancelIoEx(state.pipe.get(), &state.overlapped);
    GetOverlappedResult(state.pipe.get(), &state.overlapped, transferred, TRUE);
    return false;
  }
  return GetOverlappedResult(state.pipe.get(), &state.overlapped, transferred, FALSE) != FALSE;
}

DWORD WINAPI ReportThread(void* parameter) noexcept {
  std::unique_ptr<std::shared_ptr<ElevationReportServer::State>> handoff(
      static_cast<std::shared_ptr<ElevationReportServer::State>*>(parameter));
  ElevationReportServer::State& state = **handoff;
  DWORD transferred = 0;
  if (!state.connected) {
    // Create started the connection; nobody connects once the elevated process has ended.
    const HANDLE others[] = {state.childDone.get(), state.stop.get()};
    if (!AwaitPipe(state, others, &transferred)) return 0;
    state.connected = true;
    GetNamedPipeClientProcessId(state.pipe.get(), &state.clientProcessId);
  }
  char buffer[4096];
  for (;;) {
    state.overlapped = OVERLAPPED{};
    state.overlapped.hEvent = state.io.get();
    transferred = 0;
    if (!ReadFile(state.pipe.get(), buffer, sizeof(buffer), &transferred, &state.overlapped)) {
      if (GetLastError() != ERROR_IO_PENDING) break;  // the other end closed
      const HANDLE others[] = {state.stop.get()};
      if (!AwaitPipe(state, others, &transferred)) break;
    }
    if (transferred == 0) continue;
    const size_t room = kMaxReportBytes - std::min(kMaxReportBytes, state.bytes.size());
    try {
      state.bytes.append(buffer, std::min<size_t>(room, transferred));
    } catch (...) {
      break;
    }
    if (transferred > room) break;  // enough: the rest is dropped
  }
  return 0;
}

}  // namespace

ElevationReportServer::ElevationReportServer(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

ElevationReportServer::~ElevationReportServer() {
  if (thread_) {
    SetEvent(state_->stop.get());
    WaitForSingleObject(thread_.get(), INFINITE);
  }
}

HRESULT ElevationReportServer::Create(std::wstring_view userSid,
                                      std::unique_ptr<ElevationReportServer>* server) noexcept try {
  server->reset();
  auto state = std::make_shared<State>();
  RETURN_IF_FAILED(state->io.create(wil::EventOptions::ManualReset));
  RETURN_IF_FAILED(state->childDone.create(wil::EventOptions::ManualReset));
  RETURN_IF_FAILED(state->stop.create(wil::EventOptions::ManualReset));

  const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(userSid) + L")(A;;GA;;;BA)";
  wil::unique_hlocal_security_descriptor descriptor;
  RETURN_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      sddl.c_str(), SDDL_REVISION_1, descriptor.put(), nullptr));
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor.get(), FALSE};
  const std::wstring name = ElevationPipeName(GetCurrentProcessId());
  state->pipe.reset(CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
      static_cast<DWORD>(64 * 1024), 0, &attributes));
  if (!state->pipe) return LastErrorOr(E_FAIL);

  // Listening starts here, before anything is launched, so no early client is missed.
  state->overlapped.hEvent = state->io.get();
  if (!ConnectNamedPipe(state->pipe.get(), &state->overlapped)) {
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
      state->connected = true;
      GetNamedPipeClientProcessId(state->pipe.get(), &state->clientProcessId);
    } else if (error != ERROR_IO_PENDING) {
      return HRESULT_FROM_WIN32(error);
    }
  }

  std::unique_ptr<ElevationReportServer> created(new ElevationReportServer(state));
  auto handoff = std::make_unique<std::shared_ptr<State>>(state);
  created->thread_.reset(CreateThread(nullptr, 0, ReportThread, handoff.get(), 0, nullptr));
  if (!created->thread_) {
    const HRESULT hr = LastErrorOr(E_FAIL);
    CancelIoEx(state->pipe.get(), &state->overlapped);
    DWORD unused = 0;
    GetOverlappedResult(state->pipe.get(), &state->overlapped, &unused, TRUE);
    return hr;
  }
  handoff.release();  // the thread's now
  *server = std::move(created);
  return S_OK;
}
CATCH_RETURN();

std::vector<std::wstring> ElevationReportServer::Finish(DWORD processId,
                                                        bool* fromClient) noexcept {
  if (fromClient) *fromClient = false;
  if (thread_) {
    SetEvent(state_->childDone.get());
    if (WaitForSingleObject(thread_.get(), kFinishWaitMs) != WAIT_OBJECT_0) {
      SetEvent(state_->stop.get());
      WaitForSingleObject(thread_.get(), INFINITE);
    }
    thread_.reset();
  }
  const bool ours = state_->connected && processId != 0 && state_->clientProcessId == processId;
  if (fromClient) *fromClient = ours;
  if (!ours) return {};
  try {
    return SplitReportLines(state_->bytes);
  } catch (...) {
    return {};
  }
}

std::vector<std::wstring> SplitReportLines(std::string_view bytes) {
  std::vector<std::wstring> lines;
  while (!bytes.empty() && lines.size() < kMaxReportLines) {
    const size_t end = bytes.find('\n');
    std::string_view piece = bytes.substr(0, end);
    bytes.remove_prefix(end == std::string_view::npos ? bytes.size() : end + 1);
    if (!piece.empty() && piece.back() == '\r') piece.remove_suffix(1);
    std::wstring line = DecodeLossy(piece);
    if (line.size() > kMaxReportLineChars) line.resize(kMaxReportLineChars);
    std::replace_if(line.begin(), line.end(), IsControl, L' ');
    lines.push_back(std::move(line));
  }
  return lines;
}

// ---- starting the elevated process ----

HRESULT LaunchElevatedWithShell(const ElevationRequest& request, DWORD* processId,
                                DWORD* exitCode) noexcept try {
  *processId = 0;
  *exitCode = 0;
  // The elevated process starts in the system folder, never in a folder the caller has open
  // (it may be one that the step deletes).
  wchar_t system[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
  RETURN_LAST_ERROR_IF(length == 0 || length >= ARRAYSIZE(system));

  SHELLEXECUTEINFOW info = {sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE;
  info.hwnd = request.owner;
  info.lpVerb = L"runas";
  info.lpFile = request.exe.c_str();
  info.lpParameters = request.parameters.c_str();
  info.lpDirectory = system;
  info.nShow = SW_HIDE;
  // ERROR_CANCELLED when the user declines the prompt.
  if (!ShellExecuteExW(&info)) return LastErrorOr(E_FAIL);
  wil::unique_handle process(info.hProcess);
  RETURN_HR_IF(E_UNEXPECTED, !process);
  *processId = GetProcessId(process.get());

  if (request.owner) {
    // A window keeps answering while it waits.
    for (;;) {
      const HANDLE handle = process.get();
      const DWORD waited =
          MsgWaitForMultipleObjectsEx(1, &handle, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      if (waited == WAIT_OBJECT_0) break;
      RETURN_LAST_ERROR_IF(waited == WAIT_FAILED);
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
  } else {
    RETURN_LAST_ERROR_IF(WaitForSingleObject(process.get(), INFINITE) == WAIT_FAILED);
  }
  RETURN_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(process.get(), exitCode));
  return S_OK;
}
CATCH_RETURN();

ElevatedOutcome ClassifyElevatedExit(HRESULT launched, DWORD exitCode, HRESULT* failure) noexcept {
  *failure = S_OK;
  if (launched == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return ElevatedOutcome::kCancelled;
  if (FAILED(launched)) {
    *failure = launched;
    return ElevatedOutcome::kFailed;
  }
  if (exitCode == 0) return ElevatedOutcome::kDone;
  if (exitCode == 1) return ElevatedOutcome::kRestartPending;
  const HRESULT code = static_cast<HRESULT>(exitCode);
  *failure = FAILED(code) ? code : E_UNEXPECTED;
  return ElevatedOutcome::kFailed;
}

HRESULT RunElevated(ElevatedVerb verb, const std::filesystem::path& exe, HWND owner,
                    const ElevationLauncher& launcher, ElevatedRun* run) noexcept try {
  *run = ElevatedRun{};
  RETURN_HR_IF(E_INVALIDARG, !launcher || !exe.is_absolute() ||
                                 !IEquals(exe.filename().native(), kSettingsExe) ||
                                 ElevatedVerbName(verb).empty());
  const ElevationRequest request{exe, ElevatedParameters(verb), owner};

  // Best effort: without the pipe the step still runs, it only cannot tell what it did.
  std::unique_ptr<ElevationReportServer> server;
  std::wstring userSid;
  if (SUCCEEDED(GetCurrentUserSid(&userSid))) {
    (void)ElevationReportServer::Create(userSid, &server);
  }
  DWORD processId = 0;
  DWORD exitCode = 0;
  const HRESULT launched = launcher(request, &processId, &exitCode);
  if (server) run->report = server->Finish(processId, &run->reportChannel);
  run->exitCode = exitCode;
  run->outcome = ClassifyElevatedExit(launched, exitCode, &run->failure);

  // The elevated process writes no log of its own: this process logs what it reported.
  for (const std::wstring& line : run->report) Log(LogLevel::kInfo, kLogComponent, line);
  std::wstring summary = std::wstring(ElevatedVerbName(verb)) + L" launch=" +
                         HResultText(launched) + L" exit=" +
                         HResultText(static_cast<HRESULT>(exitCode));
  if (!run->reportChannel) summary += L" no-report";
  Log(run->outcome == ElevatedOutcome::kFailed ? LogLevel::kWarning : LogLevel::kInfo,
      kLogComponent, summary);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
