#include "sac.h"

#include <winevt.h>
// wil/resource.h defines wil::unique_evt_handle only when winevt.h came first.
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <string>
#include <utility>
#include <vector>

#include "delay_load.h"
#include "strings.h"

namespace pdft {
namespace {

// Windows paths cannot be longer than this (UNICODE_STRING counts bytes in 16 bits).
constexpr size_t kMaxPathChars = 32768;

constexpr wchar_t kCiChannel[] = L"Microsoft-Windows-CodeIntegrity/Operational";
constexpr wchar_t kFileNamePath[] = L"Event/EventData/Data[@Name='File Name']";

// Events fetched per EvtNext call, and how long one call may wait. The channel is local and
// small (1 MB by default), so the wait is generous.
constexpr DWORD kEventBatch = 64;
constexpr DWORD kEventWaitMs = 30000;

// More events than any 7 days could hold; a log that keeps going is not read without end.
constexpr int kMaxEvents = 1'000'000;

// Rendered values of one event are a few hundred bytes; a larger answer is not a file name.
constexpr DWORD kMaxRenderBytes = 64 * 1024;

HRESULT LastErrorOr(HRESULT fallback) noexcept {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : fallback;
}

// Renders the "File Name" of |event| into |buffer|; std::wstring_view{} when the event has none.
HRESULT RenderFileName(EVT_HANDLE context, EVT_HANDLE event, std::vector<BYTE>* buffer,
                       std::wstring_view* fileName) {
  *fileName = {};
  for (int attempt = 0; attempt < 2; ++attempt) {
    DWORD used = 0;
    DWORD count = 0;
    if (EvtRender(context, event, EvtRenderEventValues, static_cast<DWORD>(buffer->size()),
                  buffer->empty() ? nullptr : buffer->data(), &used, &count)) {
      if (count < 1 || used < sizeof(EVT_VARIANT)) return S_OK;
      const auto* values = reinterpret_cast<const EVT_VARIANT*>(buffer->data());
      if (values[0].Type == EvtVarTypeString && values[0].StringVal != nullptr) {
        // The string lies inside |buffer|, null-terminated; never read past the buffer's end.
        const auto* begin = reinterpret_cast<const BYTE*>(values[0].StringVal);
        const BYTE* end = buffer->data() + used;
        if (begin < buffer->data() || begin >= end) return S_OK;
        const size_t maxChars = static_cast<size_t>(end - begin) / sizeof(wchar_t);
        size_t length = 0;
        while (length < maxChars && values[0].StringVal[length] != L'\0') ++length;
        *fileName = std::wstring_view(values[0].StringVal, length);
      }
      return S_OK;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || used > kMaxRenderBytes) {
      return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA);
    }
    // EVT_VARIANT needs 8-byte alignment; a vector's storage comes from the heap, which gives
    // at least that.
    buffer->resize(used);
  }
  return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

// The body of CountCiBlocks, failing with the reason the folder could not be resolved or the log
// could not be read.
HRESULT CountMatchingEvents(const std::filesystem::path& buildFolder, int days, int* count) noexcept
    try {
  *count = 0;
  // A build folder that is gone is an answer (the count carries its error), not a failure to log.
  std::wstring deviceFolder;
  const HRESULT resolved = DeviceFolderPath(buildFolder, &deviceFolder);
  if (FAILED(resolved)) return resolved;
  RETURN_IF_FAILED(EnsureDelayLoaded("wevtapi.dll"));
  const std::wstring query = CiBlockQuery(days);
  wil::unique_evt_handle results(
      EvtQuery(nullptr, kCiChannel, query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection));
  if (!results) return LastErrorOr(E_FAIL);
  const wchar_t* paths[] = {kFileNamePath};
  wil::unique_evt_handle context(EvtCreateRenderContext(1, paths, EvtRenderContextValues));
  if (!context) return LastErrorOr(E_FAIL);

  std::vector<BYTE> buffer(1024);
  int seen = 0;
  int rendered = 0;
  int failed = 0;
  HRESULT lastFailure = S_OK;
  for (;;) {
    EVT_HANDLE batch[kEventBatch] = {};
    DWORD returned = 0;
    if (!EvtNext(results.get(), kEventBatch, batch, kEventWaitMs, 0, &returned)) {
      const DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_ITEMS) return RenderOutcome(rendered, failed, lastFailure);
      return HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT);
    }
    // Every handle EvtNext returned is closed, also when rendering one fails.
    std::vector<wil::unique_evt_handle> events;
    events.reserve(returned);
    for (DWORD i = 0; i < returned; ++i) events.emplace_back(batch[i]);
    for (const wil::unique_evt_handle& event : events) {
      // An event that cannot be rendered is passed over: one odd event (a file name too long to
      // render, say) must not cost the count of all the others. They are counted all the same,
      // so that a log none of whose events renders is not taken for one without blocks.
      std::wstring_view fileName;
      const HRESULT hr = RenderFileName(context.get(), event.get(), &buffer, &fileName);
      if (FAILED(hr)) {
        ++failed;
        lastFailure = hr;
      } else {
        ++rendered;
      }
      if (EventCountsAsBlock(hr, fileName, deviceFolder)) ++*count;
    }
    seen += static_cast<int>(returned);
    if (seen >= kMaxEvents) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
  }
}
CATCH_RETURN();

// "X:\..." below the drive's root. Build folders live on local drives; a share, a device or a
// \\?\ path is refused before anything opens it.
bool IsDriveFolder(std::wstring_view path) noexcept {
  const bool drive = path.size() > 3 &&
                     ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
  // A colon after the drive's would name a stream of a file.
  return drive && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/') &&
         path.find(L':', 2) == std::wstring_view::npos;
}

}  // namespace

SacReading ClassifySacValue(const std::optional<RegistryValue>& value) noexcept {
  SacReading reading;
  if (!value) return reading;  // kAbsent
  reading.type = value->type;
  reading.value = RegistryDword(*value);
  if (!reading.value) {
    reading.state = SacState::kUnknown;
    return reading;
  }
  switch (*reading.value) {
    case 0:
      reading.state = SacState::kOff;
      break;
    case 1:
      reading.state = SacState::kOn;
      break;
    case 2:
      reading.state = SacState::kEvaluation;
      break;
    default:
      reading.state = SacState::kUnknown;
      break;
  }
  return reading;
}

HRESULT ReadSacState(const RegistryRoots& roots, SacReading* reading) noexcept {
  *reading = {};
  std::optional<RegistryValue> value;
  RETURN_IF_FAILED(ReadRegistryValue(roots.machine, kSacPolicyKey, kSacPolicyValue, &value));
  *reading = ClassifySacValue(value);
  return S_OK;
}

std::wstring SacValueText(const SacReading& reading) {
  if (reading.value) return std::to_wstring(*reading.value);
  if (reading.type == REG_NONE) return {};
  return L"type " + std::to_wstring(reading.type);
}

HRESULT DeviceFolderPath(const std::filesystem::path& folder, std::wstring* out) noexcept try {
  out->clear();
  RETURN_HR_IF(E_INVALIDARG, !IsDriveFolder(folder.native()));
  // Attributes only, sharing everything, links followed: Code Integrity names a file by the path
  // it was finally opened at. Should a link lead to a pipe after all, its server may identify
  // this process but not act as it.
  wil::unique_hfile file(CreateFileW(
      folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  // A missing folder is an answer for the caller, not a failure worth logging.
  if (!file) return LastErrorOr(E_FAIL);
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetFinalPathNameByHandleW(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    RETURN_LAST_ERROR_IF(length == 0);
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    // Too small: |length| is the size needed, terminating null included.
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length > kMaxPathChars);
    buffer.resize(length);
  }
  while (!buffer.empty() && buffer.back() == L'\\') buffer.pop_back();
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME), buffer.empty() || buffer.front() != L'\\');
  *out = std::move(buffer);
  return S_OK;
}
CATCH_RETURN();

bool FileNameIsInFolder(std::wstring_view fileName, std::wstring_view deviceFolder) noexcept {
  while (!deviceFolder.empty() && deviceFolder.back() == L'\\') deviceFolder.remove_suffix(1);
  // Nothing left would match every name that starts with a separator.
  if (deviceFolder.empty()) return false;
  return fileName.size() > deviceFolder.size() + 1 && fileName[deviceFolder.size()] == L'\\' &&
         IStartsWith(fileName, deviceFolder);
}

bool EventCountsAsBlock(HRESULT rendered, std::wstring_view fileName,
                        std::wstring_view deviceFolder) noexcept {
  return SUCCEEDED(rendered) && FileNameIsInFolder(fileName, deviceFolder);
}

HRESULT RenderOutcome(int rendered, int failed, HRESULT lastFailure) noexcept {
  if (failed > 0 && rendered == 0) {
    return FAILED(lastFailure) ? lastFailure : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  return S_OK;
}

std::wstring CiBlockQuery(int days) {
  const long long ms = static_cast<long long>(days) * 24 * 60 * 60 * 1000;
  return L"*[System[(EventID=3077) and TimeCreated[timediff(@SystemTime) <= " +
         std::to_wstring(ms) + L"]]]";
}

BlockCount CountCiBlocks(const std::filesystem::path& buildFolder, int days) noexcept {
  BlockCount result;
  if (days < 1 || days > 365) {
    result.error = E_INVALIDARG;
    return result;
  }
  int count = 0;
  const HRESULT hr = CountMatchingEvents(buildFolder, days, &count);
  if (FAILED(hr)) {
    result.error = hr;
    return result;
  }
  result.readable = true;
  result.count = count;
  return result;
}

bool IsLoadBlockedHresult(HRESULT hr) noexcept {
  // HRESULT_FROM_WIN32 codes only: 0x8007xxxx.
  if ((static_cast<unsigned long>(hr) & 0xFFFF0000UL) != 0x80070000UL) return false;
  const unsigned long code = static_cast<unsigned long>(hr) & 0xFFFFUL;
  return code == ERROR_INVALID_IMAGE_HASH ||                 // 577
         code == ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION ||  // 4551
         (code >= 4556 && code <= 4559) || (code >= 4580 && code <= 4583);
}

bool ShouldCountBlocks(SacState state, bool dllRegistered, std::optional<HRESULT> probe) noexcept {
  return state == SacState::kOn && dllRegistered && probe != S_OK;
}

bool ShouldRecountBlocks(std::optional<HRESULT> probe, const BlockCount& first) noexcept {
  const bool noBlocks = !first.readable || first.count == 0;
  return noBlocks && probe.has_value() && *probe != S_OK && !IsLoadBlockedHresult(*probe);
}

SacVerdict DecideSac(SacState state, std::optional<HRESULT> probe,
                     const std::optional<BlockCount>& blocks) noexcept {
  SacVerdict verdict;
  switch (state) {
    case SacState::kEvaluation:
      verdict.kind = SacVerdictKind::kEval;
      return verdict;
    case SacState::kUnknown:
      verdict.kind = SacVerdictKind::kUnknown;
      return verdict;
    case SacState::kOff:
    case SacState::kAbsent:
      return verdict;  // kNone
    case SacState::kOn:
      break;
  }
  if (probe == S_OK) {
    verdict.kind = SacVerdictKind::kOk;
    return verdict;
  }
  if (blocks && blocks->readable && blocks->count > 0) {
    verdict.kind = SacVerdictKind::kBlocked;
    verdict.blocks = blocks->count;
    return verdict;
  }
  if (probe && IsLoadBlockedHresult(*probe)) {
    verdict.kind = SacVerdictKind::kBlocked;
    verdict.evidence = probe;
    return verdict;
  }
  verdict.kind = !blocks || blocks->readable ? SacVerdictKind::kOn : SacVerdictKind::kOnUnknown;
  return verdict;
}

}  // namespace pdft
