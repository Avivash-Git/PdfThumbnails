#include "elevation.h"

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <cwchar>
#include <memory>
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

// How a report line about a part the step did not complete starts (NotDoneLine).
constexpr std::wstring_view kNotDonePrefix = L"not done: ";

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

// The RID of the label a TokenIntegrityLevel query returned; 0 for a SID without one.
DWORD LabelRid(const TOKEN_MANDATORY_LABEL& label) noexcept {
  const PSID sid = label.Label.Sid;
  const UCHAR count = *GetSidSubAuthorityCount(sid);
  return count ? *GetSidSubAuthority(sid, count - 1u) : 0;
}

// The facts CheckElevatedEntry judges, read from this process.
HRESULT GatherEntryFacts(ElevatedEntryFacts* facts) noexcept try {
  *facts = ElevatedEntryFacts{};
  facts->devInstall = kDevInstall;
  RETURN_IF_FAILED(ReadEntryTokenFacts(nullptr, facts));

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

// One report line per folder of a removal: "<what> <folder name>"; and why none went when the
// class for all users is one the steps for all users leave alone.
void ReportRemoval(ElevatedReporter& report, const MachineRemoval& removal) {
  for (const std::wstring& name : removal.removed) report.Line(L"removed " + name);
  for (const std::wstring& name : removal.left) report.Line(L"in use, left " + name);
  for (const std::wstring& name : removal.failed) report.Line(L"failed " + name);
  if (removal.classForeign) {
    report.Line(
        L"the class for all users names its DLL with environment variables (REG_EXPAND_SZ); no "
        L"build folder was removed");
  }
}

// machine-migrate-v1: 1.x's build folders for all users, except the one the machine-wide class
// still uses. In-use files go to a trash folder next to the install folder (same volume).
HRESULT RunMachineMigrateV1(ElevatedReporter& report, bool* restartPending,
                            std::vector<std::wstring>* notDone) noexcept try {
  notDone->clear();
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
  if (FAILED(removal.firstError)) {
    notDone->push_back(L"migrate-v1: " + HResultText(removal.firstError));
  }
  RETURN_IF_FAILED(hr);
  // The failures it went on after are listed above (ReportRemoval) and reported as not done; it
  // ends with the first one's HRESULT, as before.
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
  facts->integrityRid = LabelRid(*reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label));

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

HRESULT ReadEntryTokenFacts(HANDLE token, ElevatedEntryFacts* facts) noexcept {
  facts->tokenElevated = false;
  facts->administratorsEnabled = false;
  facts->integrityRid = 0;
  // Without a token: the pseudo handle of this thread's current token (its impersonation token,
  // else the process's), the one CheckTokenMembership(nullptr) judges too.
  const HANDLE source = token ? token : GetCurrentThreadEffectiveToken();
  TOKEN_ELEVATION elevation = {};
  DWORD size = 0;
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(source, TokenElevation, &elevation, sizeof(elevation), &size));
  alignas(TOKEN_MANDATORY_LABEL) BYTE label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(source, TokenIntegrityLevel, label, sizeof(label), &size));

  // Enabled, not merely present: CheckTokenMembership ignores a deny-only or disabled group.
  BYTE administrators[SECURITY_MAX_SID_SIZE];
  DWORD administratorsSize = sizeof(administrators);
  RETURN_IF_WIN32_BOOL_FALSE(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                                administrators, &administratorsSize));
  BOOL member = FALSE;
  if (token) {
    // CheckTokenMembership takes an impersonation token only.
    wil::unique_handle identification;
    RETURN_IF_WIN32_BOOL_FALSE(DuplicateToken(token, SecurityIdentification, identification.put()));
    RETURN_IF_WIN32_BOOL_FALSE(CheckTokenMembership(identification.get(), administrators, &member));
  } else {
    RETURN_IF_WIN32_BOOL_FALSE(CheckTokenMembership(nullptr, administrators, &member));
  }
  facts->tokenElevated = elevation.TokenIsElevated != 0;
  facts->administratorsEnabled = member != FALSE;
  facts->integrityRid = LabelRid(*reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label));
  return S_OK;
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

ElevatedRefusal FindElevatedRefusal(const ElevatedEntryFacts& facts) noexcept {
  // Administrator rights first: an elevated token alone does not prove them.
  if (!facts.tokenElevated) return ElevatedRefusal::kNotElevated;
  if (!facts.administratorsEnabled) return ElevatedRefusal::kAdministratorsNotEnabled;
  if (facts.integrityRid < static_cast<DWORD>(SECURITY_MANDATORY_HIGH_RID)) {
    return ElevatedRefusal::kIntegrityBelowHigh;
  }
  if (facts.packaged) return ElevatedRefusal::kPackaged;
  if (!facts.verb) return ElevatedRefusal::kNotOneStep;
  const bool exempt = facts.devInstall && *facts.verb == ElevatedVerb::kMachineInstall;
  if (!facts.runsFromMachineRoot && !exempt) return ElevatedRefusal::kNotFromMachineRoot;
  if (FAILED(facts.reparsePoints)) return ElevatedRefusal::kReparsePoint;
  return ElevatedRefusal::kNone;
}

HRESULT CheckElevatedEntry(const ElevatedEntryFacts& facts) noexcept {
  switch (FindElevatedRefusal(facts)) {
    case ElevatedRefusal::kNone:
      return S_OK;
    case ElevatedRefusal::kNotElevated:
    case ElevatedRefusal::kAdministratorsNotEnabled:
    case ElevatedRefusal::kIntegrityBelowHigh:
      return kNoAdministratorRightsHr;
    case ElevatedRefusal::kPackaged:
      return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED_IN_APPCONTAINER);
    case ElevatedRefusal::kNotOneStep:
      return E_INVALIDARG;
    case ElevatedRefusal::kNotFromMachineRoot:
      return E_ACCESSDENIED;
    case ElevatedRefusal::kReparsePoint:
      // A failure, so the step never starts on it (FindElevatedRefusal names it only then).
      return facts.reparsePoints;
  }
  return E_UNEXPECTED;
}

std::wstring_view ElevatedRefusalText(ElevatedRefusal refusal) noexcept {
  switch (refusal) {
    case ElevatedRefusal::kNone:
      break;
    case ElevatedRefusal::kNotElevated:
      return L"no administrator rights (not elevated)";
    case ElevatedRefusal::kAdministratorsNotEnabled:
      return L"no administrator rights (the Administrators group is not enabled)";
    case ElevatedRefusal::kIntegrityBelowHigh:
      return L"no administrator rights (an integrity level below High)";
    case ElevatedRefusal::kPackaged:
      return L"inside or below an app package";
    case ElevatedRefusal::kNotOneStep:
      return L"not exactly one known step";
    case ElevatedRefusal::kNotFromMachineRoot:
      return L"not the settings executable installed for all users";
    case ElevatedRefusal::kReparsePoint:
      return L"a link or junction on the way to the install folder";
  }
  return {};
}

int ElevatedMain() noexcept {
  ElevatedEntryFacts facts;
  HRESULT hr = GatherEntryFacts(&facts);
  ElevatedRefusal refusal = ElevatedRefusal::kNone;
  if (SUCCEEDED(hr)) {
    refusal = FindElevatedRefusal(facts);
    hr = CheckElevatedEntry(facts);
  }
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
      const std::wstring why = refusal == ElevatedRefusal::kNone
                                   ? std::wstring(L"its own facts could not be read")
                                   : std::wstring(ElevatedRefusalText(refusal));
      report.Line(L"elevated " + std::wstring(name) + L" refused hr=" + HResultText(hr) + L": " +
                  why + L" (" + tokenText + L")");
      return static_cast<int>(hr);
    }
    report.Line(L"elevated " + std::wstring(name) + L" start (" + tokenText + L")");
  } catch (...) {
    return static_cast<int>(FAILED(hr) ? hr : E_OUTOFMEMORY);
  }
  bool restartPending = false;
  std::vector<std::wstring> notDone;
  // Where the step's failure started: the functions on its way note it (failure_trace.h).
  FailureTrace trace;
  hr = RunElevatedStep(*facts.verb, report, &restartPending, &notDone);
  return EndElevatedStep(report, *facts.verb, hr, restartPending, notDone, trace);
}

HRESULT RunElevatedStep(ElevatedVerb verb, ElevatedReporter& report, bool* restartPending,
                        std::vector<std::wstring>* notDone) noexcept {
  *restartPending = false;
  notDone->clear();
  switch (verb) {
    case ElevatedVerb::kMachineMigrateV1:
      return RunMachineMigrateV1(report, restartPending, notDone);
    case ElevatedVerb::kMachineInstall:
      // The parser and the entry checks keep it out of builds without the developer install.
      if (!kDevInstall) break;
      return RunMachineInstall(report, restartPending, notDone);
    case ElevatedVerb::kMachineRepairClass:
      // One action, no parts: it is done or it failed.
      return RunMachineRepairClass(report);
    case ElevatedVerb::kMachineUninstall:
      return RunMachineUninstall(report, restartPending, notDone);
  }
  return E_NOTIMPL;
}

// ---- reporting channel ----

std::wstring ElevationPipeName(DWORD parentProcessId) {
  return std::wstring(L"\\\\.\\pipe\\") + kObjectPrefix + L".Elevated." +
         std::to_wstring(parentProcessId);
}

bool TrustReportServer(const ReportServerFacts& facts) noexcept {
  return facts.parentId != 0 && facts.serverId == facts.parentId &&
         facts.serverSession == facts.parentSession && facts.ownerIsParentsUser;
}

namespace {

// The facts TrustReportServer judges: those of |pipe| (opened with READ_CONTROL) and of the
// parent |parentId|, read through |parent| (PROCESS_QUERY_LIMITED_INFORMATION), which keeps the
// id the parent's while they are read.
HRESULT ReadReportServerFacts(HANDLE pipe, DWORD parentId, HANDLE parent,
                              ReportServerFacts* facts) noexcept {
  *facts = ReportServerFacts{};
  facts->parentId = parentId;
  RETURN_IF_WIN32_BOOL_FALSE(ProcessIdToSessionId(parentId, &facts->parentSession));
  RETURN_IF_WIN32_BOOL_FALSE(GetNamedPipeServerProcessId(pipe, &facts->serverId));
  RETURN_IF_WIN32_BOOL_FALSE(GetNamedPipeServerSessionId(pipe, &facts->serverSession));

  PSID owner = nullptr;
  wil::unique_hlocal_security_descriptor descriptor;
  RETURN_IF_WIN32_ERROR(GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                        nullptr, nullptr, nullptr, descriptor.put()));
  wil::unique_handle token;
  RETURN_IF_WIN32_BOOL_FALSE(OpenProcessToken(parent, TOKEN_QUERY, token.put()));
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(token.get(), TokenUser, user, sizeof(user), &size));
  facts->ownerIsParentsUser =
      owner != nullptr &&
      EqualSid(owner, reinterpret_cast<const TOKEN_USER*>(user)->User.Sid) != FALSE;
  return S_OK;
}

// Writes |bytes| to |pipe| (opened for overlapped I/O; |event| a manual-reset event for it).
// True when all of them went in within kReportWriteTimeoutMs. A write still pending then is
// cancelled, and waited for until it has ended, since until then it uses |bytes|.
bool WriteInTime(HANDLE pipe, HANDLE event, std::string_view bytes) noexcept {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = event;
  if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &overlapped) &&
      GetLastError() != ERROR_IO_PENDING) {
    return false;
  }
  const bool inTime = WaitForSingleObject(event, kReportWriteTimeoutMs) == WAIT_OBJECT_0;
  if (!inTime) CancelIoEx(pipe, &overlapped);
  DWORD written = 0;
  const bool ended = GetOverlappedResult(pipe, &overlapped, &written, TRUE) != FALSE;
  return inTime && ended && written == bytes.size();
}

}  // namespace

ElevatedReporter ElevatedReporter::Connect() noexcept {
  ElevatedReporter reporter;
  // The parent stays open until the checks are done: meanwhile its id cannot pass to another
  // process, so what is checked against the id is checked against the parent.
  DWORD parentId = 0;
  wil::unique_handle parent;
  if (FAILED(OpenParentProcess(&parentId, std::addressof(parent)))) return reporter;
  std::wstring name;
  try {
    name = ElevationPipeName(parentId);
  } catch (...) {
    return reporter;
  }
  // Anonymous: whoever serves the pipe cannot impersonate this (elevated) process. READ_CONTROL
  // to read the pipe's owner; overlapped, so that a write the pipe does not take can be given up.
  wil::unique_hfile pipe(
      CreateFileW(name.c_str(), GENERIC_WRITE | READ_CONTROL, 0, nullptr, OPEN_EXISTING,
                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
  if (!pipe) return reporter;
  ReportServerFacts facts;
  if (FAILED(ReadReportServerFacts(pipe.get(), parentId, parent.get(), &facts)) ||
      !TrustReportServer(facts)) {
    return reporter;
  }
  if (FAILED(reporter.written_.create(wil::EventOptions::ManualReset))) return reporter;
  reporter.pipe_ = std::move(pipe);
  return reporter;
}

void ElevatedReporter::Line(std::wstring_view text) noexcept {
  if (!pipe_) return;
  std::string bytes;
  try {
    std::wstring clean(text.substr(0, kMaxReportLineChars));
    std::replace_if(clean.begin(), clean.end(), IsControl, L' ');
    bytes = WideToUtf8Lossy(clean);
    bytes.push_back('\n');
  } catch (...) {
    return;  // out of memory: this line is lost, the next ones may still go
  }
  // A parent that does not take the line in time is given up on: the step goes on unreported
  // rather than wait for it.
  if (!WriteInTime(pipe_.get(), written_.get(), bytes)) pipe_.reset();
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
    // Finish or the destructor stopped waiting: what the client still sends is dropped. Checked
    // on every pass, because a client that keeps writing makes each read complete at once, and
    // only a read that waits looks at |stop| otherwise.
    if (WaitForSingleObject(state.stop.get(), 0) == WAIT_OBJECT_0) break;
    state.overlapped = OVERLAPPED{};
    state.overlapped.hEvent = state.io.get();
    transferred = 0;
    if (!ReadFile(state.pipe.get(), buffer, sizeof(buffer), &transferred, &state.overlapped)) {
      if (GetLastError() != ERROR_IO_PENDING) break;  // the other end closed
      const HANDLE others[] = {state.stop.get()};
      if (!AwaitPipe(state, others, &transferred)) break;
    }
    if (transferred == 0) continue;
    // Past the cap the rest is still read, and dropped: left in the pipe, it would make the
    // elevated process wait for room.
    const size_t room = kMaxReportBytes - std::min(kMaxReportBytes, state.bytes.size());
    try {
      state.bytes.append(buffer, std::min<size_t>(room, transferred));
    } catch (...) {
      // Out of memory: dropped like the bytes past the cap, and reading goes on.
    }
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

  // Owned by the user, whatever the token's default owner (Administrators in some elevated
  // tokens): the elevated side writes only to a pipe its parent's user owns.
  const std::wstring sddl =
      L"O:" + std::wstring(userSid) + L"D:P(A;;GA;;;" + std::wstring(userSid) + L")(A;;GA;;;BA)";
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

// ---- parts a step did not complete ----

namespace {

// The longest part name a report line may give.
constexpr size_t kMaxPartChars = 64;

// "0x" and exactly 8 hex digits, in either case: HResultText's form.
std::optional<HRESULT> ParseHResultText(std::wstring_view text) noexcept {
  if (text.size() != 10 || !text.starts_with(L"0x")) return std::nullopt;
  unsigned long value = 0;
  for (const wchar_t c : text.substr(2)) {
    unsigned long digit = 0;
    if (c >= L'0' && c <= L'9') {
      digit = static_cast<unsigned long>(c - L'0');
    } else if (c >= L'a' && c <= L'f') {
      digit = static_cast<unsigned long>(c - L'a') + 10;
    } else if (c >= L'A' && c <= L'F') {
      digit = static_cast<unsigned long>(c - L'A') + 10;
    } else {
      return std::nullopt;
    }
    value = value * 16 + digit;
  }
  return static_cast<HRESULT>(value);
}

// A part's name as a report line may give it: 1 to kMaxPartChars characters of a-z, 0-9, "-"
// and space, with no space first or last. Nothing else reaches the command's failures.
bool IsPartName(std::wstring_view name) noexcept {
  if (name.empty() || name.size() > kMaxPartChars || name.front() == L' ' || name.back() == L' ') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L' ';
  });
}

// The part |line| names as not done (a NotDoneLine whose part and HRESULT are well formed, the
// HRESULT a failure); std::nullopt for any other line. May throw std::bad_alloc.
std::optional<NotDonePart> ParseNotDoneLine(std::wstring_view line) {
  if (!line.starts_with(kNotDonePrefix)) return std::nullopt;
  line.remove_prefix(kNotDonePrefix.size());
  // At the last ": ": the HRESULT is the last word.
  const size_t split = line.rfind(L": ");
  if (split == std::wstring_view::npos) return std::nullopt;
  const std::wstring_view part = line.substr(0, split);
  const std::optional<HRESULT> hr = ParseHResultText(line.substr(split + 2));
  if (!IsPartName(part) || !hr || SUCCEEDED(*hr)) return std::nullopt;
  return NotDonePart{std::wstring(part), *hr};
}

// Whether |line| is the end line of the step |verb| (ReadReportParts says how it is read), with
// its not-done count, when it has one, in |counted|. May throw std::bad_alloc.
bool ParseEndLine(std::wstring_view line, ElevatedVerb verb, std::optional<size_t>* counted) {
  counted->reset();
  const std::wstring start = L"elevated " + std::wstring(ElevatedVerbName(verb)) + L" end hr=";
  if (!line.starts_with(start)) return false;
  line.remove_prefix(start.size());
  if (!ParseHResultText(line.substr(0, 10))) return false;
  line.remove_prefix(10);
  constexpr std::wstring_view kCount = L"not-done=";
  std::optional<size_t> count;
  // The words after it, each after a space; those this build does not know are skipped.
  while (!line.empty()) {
    if (line.front() != L' ') return false;
    line.remove_prefix(1);
    const std::wstring_view word = line.substr(0, line.find(L' '));
    line.remove_prefix(word.size());
    if (!word.starts_with(kCount)) continue;
    const std::wstring_view digits = word.substr(kCount.size());
    if (digits.empty() || digits.size() > 9 ||
        !std::all_of(digits.begin(), digits.end(),
                     [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
      continue;
    }
    size_t value = 0;
    for (const wchar_t c : digits) value = value * 10 + static_cast<size_t>(c - L'0');
    // A count past what a report can hold names no more parts than that.
    count = std::min(value, kMaxReportLines);
  }
  *counted = count;
  return true;
}

// A step that never ran: declined, not launched, or refused for want of administrator rights.
// What came back from it is not a part of it.
bool StepNeverRan(const ElevatedRun& run) noexcept {
  return run.outcome == ElevatedOutcome::kCancelled || FAILED(run.launched) ||
         LackedAdministratorRights(run);
}

}  // namespace

std::wstring NotDoneLine(std::wstring_view failure) {
  return std::wstring(kNotDonePrefix) + std::wstring(failure);
}

std::wstring ElevatedEndLine(ElevatedVerb verb, HRESULT hr, bool restartPending, size_t notDone) {
  return L"elevated " + std::wstring(ElevatedVerbName(verb)) + L" end hr=" + HResultText(hr) +
         (restartPending ? L" restart-pending" : L"") + L" not-done=" + std::to_wstring(notDone);
}

int EndElevatedStep(ElevatedReporter& report, ElevatedVerb verb, HRESULT hr, bool restartPending,
                    std::span<const std::wstring> notDone, const FailureTrace& trace) noexcept {
  try {
    for (const std::wstring& failure : notDone) report.Line(NotDoneLine(failure));
    if (FAILED(hr)) {
      std::wstring where = trace.failure() ? TracedFailureText(*trace.failure())
                                           : std::wstring(L"at a place not traced");
      // A caller may pass a failure on as another HRESULT; the end line says which.
      if (trace.failure() && trace.failure()->hr != hr) where += L" (end hr differs)";
      report.Line(L"elevated " + std::wstring(ElevatedVerbName(verb)) + L" failed " + where);
    }
    report.Line(ElevatedEndLine(verb, hr, restartPending, notDone.size()));
  } catch (...) {
  }
  if (FAILED(hr)) return static_cast<int>(hr);
  return restartPending ? 1 : 0;
}

ReportParts ReadReportParts(ElevatedVerb verb, std::span<const std::wstring> lines) {
  ReportParts parts;
  std::span<const std::wstring> before = lines;
  // Only as the last line: what came after an end line is not this step's report.
  if (!lines.empty() && ParseEndLine(lines.back(), verb, &parts.counted)) {
    parts.ended = true;
    before = lines.first(lines.size() - 1);
  }
  for (const std::wstring& line : before) {
    if (std::optional<NotDonePart> part = ParseNotDoneLine(line)) {
      parts.named.push_back(std::move(*part));
    }
  }
  return parts;
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

bool LackedAdministratorRights(const ElevatedRun& run) noexcept {
  return run.outcome == ElevatedOutcome::kFailed && SUCCEEDED(run.launched) &&
         run.failure == kNoAdministratorRightsHr;
}

bool ReportChannelTaken(const ElevatedRun& run) noexcept {
  // A first instance fails with ERROR_ACCESS_DENIED where a pipe of the name exists already (so
  // does an instance that pipe's DACL refuses), with ERROR_PIPE_BUSY where it takes no more.
  return run.reportServer == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
         run.reportServer == HRESULT_FROM_WIN32(ERROR_PIPE_BUSY);
}

size_t CountPartsNotDone(const ElevatedRun& run) noexcept {
  if (StepNeverRan(run)) return 0;
  return std::max(run.parts.named.size(), run.parts.counted.value_or(0));
}

bool PartsNotDone(const ElevatedRun& run) noexcept { return CountPartsNotDone(run) > 0; }

std::vector<std::wstring> NotDoneFailures(const ElevatedRun& run, std::wstring_view step) {
  std::vector<std::wstring> failures;
  if (StepNeverRan(run)) return failures;
  for (const NotDonePart& part : run.parts.named) {
    failures.push_back(std::wstring(step) + L" " + part.part + L": " + HResultText(part.hr));
  }
  // Counted, but without a line this build can read (a later build's part, or a line lost): one
  // entry for all of them, so that none goes unsaid.
  const size_t unnamed = CountPartsNotDone(run) - run.parts.named.size();
  if (unnamed > 0) {
    failures.push_back(std::wstring(step) + L" unnamed parts: " + std::to_wstring(unnamed) +
                       L" (see the step's report)");
  }
  return failures;
}

bool NeedsStepFailure(const ElevatedRun& run) noexcept {
  if (run.outcome != ElevatedOutcome::kFailed || LackedAdministratorRights(run)) return false;
  // A launch that failed ran nothing, so nothing it reported is a part of it.
  if (FAILED(run.launched) || run.parts.named.empty()) return true;
  return run.parts.named.front().hr != run.failure;
}

bool ReportIncomplete(const ElevatedRun& run) noexcept {
  return (run.outcome == ElevatedOutcome::kDone ||
          run.outcome == ElevatedOutcome::kRestartPending) &&
         !run.parts.ended;
}

std::wstring ElevatedRunSummary(ElevatedVerb verb, const ElevatedRun& run) {
  std::wstring summary = std::wstring(ElevatedVerbName(verb)) + L" launch=" +
                         HResultText(run.launched) + L" exit=" +
                         HResultText(static_cast<HRESULT>(run.exitCode));
  if (FAILED(run.reportServer)) {
    summary += L" report-channel=" + HResultText(run.reportServer) +
               (ReportChannelTaken(run) ? L" (taken by another process)" : L"");
  }
  if (!run.reportChannel) {
    summary += L" no-report";
  } else if (ReportIncomplete(run)) {
    summary += L" report-incomplete";
  }
  if (const size_t count = CountPartsNotDone(run); count > 0) {
    summary += L" not-done=" + std::to_wstring(count);
  }
  return summary;
}

HRESULT RunElevated(ElevatedVerb verb, const std::filesystem::path& exe, HWND owner,
                    const ElevationLauncher& launcher, ElevatedRun* run) noexcept try {
  *run = ElevatedRun{};
  RETURN_HR_IF(E_INVALIDARG, !launcher || !exe.is_absolute() ||
                                 !IEquals(exe.filename().native(), kSettingsExe) ||
                                 ElevatedVerbName(verb).empty());
  const ElevationRequest request{exe, ElevatedParameters(verb), owner};

  // Best effort: without the pipe the step still runs, it only cannot tell what it did. Why there
  // is none is kept, for the log and for the caller: a name someone else took first is worth
  // telling the user.
  std::unique_ptr<ElevationReportServer> server;
  std::wstring userSid;
  run->reportServer = GetCurrentUserSid(&userSid);
  if (SUCCEEDED(run->reportServer)) {
    run->reportServer = ElevationReportServer::Create(userSid, &server);
  }
  DWORD processId = 0;
  DWORD exitCode = 0;
  const HRESULT launched = launcher(request, &processId, &exitCode);
  if (server) run->report = server->Finish(processId, &run->reportChannel);
  run->parts = ReadReportParts(verb, run->report);
  run->exitCode = exitCode;
  run->launched = launched;
  run->outcome = ClassifyElevatedExit(launched, exitCode, &run->failure);

  // The elevated process writes no log of its own: this process logs what it reported, a part it
  // did not complete as a warning.
  for (const std::wstring& line : run->report) {
    Log(line.starts_with(kNotDonePrefix) ? LogLevel::kWarning : LogLevel::kInfo, kLogComponent,
        line);
  }
  const bool warn = run->outcome == ElevatedOutcome::kFailed || ReportChannelTaken(*run) ||
                    PartsNotDone(*run) || ReportIncomplete(*run);
  Log(warn ? LogLevel::kWarning : LogLevel::kInfo, kLogComponent, ElevatedRunSummary(verb, *run));
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
