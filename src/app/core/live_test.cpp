#include "live_test.h"

#include <objbase.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <cstdio>
#include <memory>
#include <system_error>

#include "paths.h"
#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

static_assert((kLiveTestFlags & WTS_EXTRACTINPROC) == 0,
              "the Explorer test must never extract in this process");
static_assert(kLiveTestFlags ==
              (WTS_FORCEEXTRACTION | WTS_EXTRACTDONOTCACHE | WTS_INSTANCESURROGATE));

// The handler's largest edge; Explorer never asks for more.
constexpr UINT kMaxThumbnailSize = 2560;

constexpr wchar_t kProbeInfix[] = L"-probe-";
constexpr wchar_t kProbeExtension[] = L".pdf";
constexpr size_t kGuidChars = 36;  // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

// What the caller and the worker thread share. The caller and the thread each hold a reference
// (std::shared_ptr), so the block lives until the later of the two lets go: after a timeout the
// caller returns and drops its reference, and the worker, still inside the shell, keeps writing
// into memory that stays valid until it ends. The caller reads |result| only after |finished| is
// set, which orders the worker's writes before the read.
struct LiveTestJob {
  std::wstring file;
  UINT size = 0;
  bool ownsFile = false;  // a probe written for this test: the worker deletes it when done
  LiveTest result;
  wil::unique_event_nothrow finished;
};

DWORD ElapsedMs(const LARGE_INTEGER& start, const LARGE_INTEGER& frequency) noexcept {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const long long ms = (now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;
  return ms < 0 ? 0 : ms > MAXDWORD ? MAXDWORD : static_cast<DWORD>(ms);
}

// The extraction itself, on the worker thread, in an apartment of its own.
void Extract(LiveTestJob* job) noexcept {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(init)) {
    job->result.hr = init;
    return;
  }
  {
    // Every COM pointer goes before CoUninitialize below.
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    wil::com_ptr_nothrow<IThumbnailCache> cache;
    HRESULT hr = CoCreateInstance(__uuidof(LocalThumbnailCache), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(cache.put()));
    wil::com_ptr_nothrow<IShellItem> item;
    if (SUCCEEDED(hr)) {
      hr = SHCreateItemFromParsingName(job->file.c_str(), nullptr, IID_PPV_ARGS(item.put()));
    }
    wil::com_ptr_nothrow<ISharedBitmap> bitmap;
    if (SUCCEEDED(hr)) {
      hr = cache->GetThumbnail(item.get(), job->size, static_cast<WTS_FLAGS>(kLiveTestFlags),
                               bitmap.put(), nullptr, nullptr);
    }
    job->result.ms = ElapsedMs(start, frequency);
    if (bitmap) {
      SIZE size{};
      if (SUCCEEDED(bitmap->GetSize(&size))) job->result.size = size;
    }
    job->result.hr = hr;
  }
  CoUninitialize();
}

DWORD WINAPI LiveTestThread(void* parameter) noexcept {
  std::shared_ptr<LiveTestJob> job;
  {
    // RunLiveTest handed over a reference of its own on the heap; it is the thread's now.
    std::unique_ptr<std::shared_ptr<LiveTestJob>> handoff(
        static_cast<std::shared_ptr<LiveTestJob>*>(parameter));
    job = std::move(*handoff);
  }
  Extract(job.get());
  // The surrogate has let go of the file once GetThumbnail returned.
  if (job->ownsFile) DeleteFileW(job->file.c_str());
  SetEvent(job->finished.get());
  return 0;
}

// %TEMP% (GetTempPathW), without the trailing backslash it comes with.
HRESULT TempDirectory(std::filesystem::path* dir) {
  dir->clear();
  std::wstring buffer(MAX_PATH + 1, L'\0');
  for (int attempt = 0; attempt < 3; ++attempt) {
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    RETURN_LAST_ERROR_IF(length == 0);
    if (length < buffer.size()) {
      buffer.resize(length);
      while (buffer.size() > 3 && buffer.back() == L'\\') buffer.pop_back();
      *dir = std::move(buffer);
      return S_OK;
    }
    buffer.assign(length + 1, L'\0');  // |length| includes the null then
  }
  return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

bool IsHexDigit(wchar_t c) noexcept {
  return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

bool IsGuidWithoutBraces(std::wstring_view text) noexcept {
  if (text.size() != kGuidChars) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? text[i] != L'-' : !IsHexDigit(text[i])) return false;
  }
  return true;
}

ULONGLONG Ticks(const FILETIME& time) noexcept {
  return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

}  // namespace

HRESULT RunLiveTest(UINT size, const std::filesystem::path* file, DWORD timeoutMs,
                    LiveTest* result) noexcept try {
  *result = {};
  RETURN_HR_IF(E_INVALIDARG, size == 0 || size > kMaxThumbnailSize);
  auto job = std::make_shared<LiveTestJob>();
  job->size = size;
  RETURN_IF_FAILED(job->finished.create(wil::EventOptions::ManualReset));
  if (file) {
    job->file = file->native();
  } else {
    std::filesystem::path probe;
    RETURN_IF_FAILED(WriteProbePdf(&probe));
    job->file = probe.native();
    job->ownsFile = true;
  }
  auto handoff = std::make_unique<std::shared_ptr<LiveTestJob>>(job);
  wil::unique_handle thread(CreateThread(nullptr, 0, LiveTestThread, handoff.get(), 0, nullptr));
  if (!thread) {
    const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    if (job->ownsFile) DeleteFileW(job->file.c_str());
    RETURN_HR(FAILED(hr) ? hr : E_FAIL);
  }
  handoff.release();  // LiveTestThread deletes it
  const DWORD wait = WaitForSingleObject(job->finished.get(), timeoutMs);
  if (wait == WAIT_OBJECT_0) {
    *result = job->result;
    return S_OK;
  }
  if (wait == WAIT_TIMEOUT) {
    result->hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    result->ms = timeoutMs;
    return S_OK;
  }
  RETURN_LAST_ERROR();
}
CATCH_RETURN();

LiveTestSkip DecideLiveTestSkip(const LiveTestConditions& conditions) noexcept {
  if (!conditions.requested) return LiveTestSkip::kNotRequested;
  if (conditions.elevated) return LiveTestSkip::kElevated;
  if (conditions.virtualized) return LiveTestSkip::kVirtualized;
  if (!conditions.testFile && !conditions.effectiveIsOurs) return LiveTestSkip::kNotOurHandler;
  if (!conditions.testFile && !conditions.classRegistered) return LiveTestSkip::kNoClass;
  return LiveTestSkip::kNone;
}

std::string ProbePdfBytes() {
  // Built the way 1.x built it, so the offsets in the xref table are computed, not typed.
  static constexpr const char* kObjects[] = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>",
  };
  std::string pdf = "%PDF-1.4\n";
  size_t offsets[std::size(kObjects)] = {};
  for (size_t i = 0; i < std::size(kObjects); ++i) {
    offsets[i] = pdf.size();
    pdf += std::to_string(i + 1) + " 0 obj\n" + kObjects[i] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 4\n0000000000 65535 f \n";
  for (const size_t offset : offsets) {
    char entry[32] = {};
    std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
    pdf += entry;
  }
  pdf += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

std::wstring ProbeFileName(const GUID& id) {
  wchar_t guid[kGuidChars + 1] = {};
  std::swprintf(guid, std::size(guid), L"%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned long>(id.Data1), id.Data2, id.Data3, id.Data4[0], id.Data4[1],
                id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
  return std::wstring(kProductName) + kProbeInfix + guid + kProbeExtension;
}

bool IsProbeFileName(std::wstring_view name) noexcept {
  const std::wstring_view product = kProductName;
  const std::wstring_view infix = kProbeInfix;
  const std::wstring_view extension = kProbeExtension;
  const size_t prefix = product.size() + infix.size();
  if (name.size() != prefix + kGuidChars + extension.size()) return false;
  return IEquals(name.substr(0, product.size()), product) &&
         IEquals(name.substr(product.size(), infix.size()), infix) &&
         IsGuidWithoutBraces(name.substr(prefix, kGuidChars)) &&
         IEquals(name.substr(prefix + kGuidChars), extension);
}

HRESULT WriteProbePdf(std::filesystem::path* path) noexcept try {
  path->clear();
  std::filesystem::path dir;
  RETURN_IF_FAILED(TempDirectory(&dir));
  GUID id;
  RETURN_IF_FAILED(CoCreateGuid(&id));
  std::filesystem::path probe = dir / ProbeFileName(id);
  const std::string bytes = ProbePdfBytes();
  // CREATE_NEW: never over a file (or a link) that is already there.
  wil::unique_hfile file(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
  RETURN_LAST_ERROR_IF(!file);
  DWORD written = 0;
  if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
      written != bytes.size()) {
    const DWORD error = GetLastError();
    file.reset();
    DeleteFileW(probe.c_str());
    RETURN_HR(HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT));
  }
  file.reset();
  *path = std::move(probe);
  return S_OK;
}
CATCH_RETURN();

bool IsStaleProbe(const FILETIME& written, const FILETIME& now) noexcept {
  const ULONGLONG at = Ticks(written);
  const ULONGLONG current = Ticks(now);
  return at < current && current - at > kStaleProbeAge100ns;
}

HRESULT SweepStaleProbes(const std::filesystem::path& directory, int* removed) noexcept try {
  if (removed) *removed = 0;
  std::filesystem::path dir = directory;
  if (dir.empty()) RETURN_IF_FAILED(TempDirectory(&dir));
  const std::filesystem::path pattern = dir / (std::wstring(kProductName) + kProbeInfix + L"*");
  WIN32_FIND_DATAW entry = {};
  wil::unique_hfind find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry,
                                          FindExSearchNameMatch, nullptr, 0));
  if (!find) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_OK;
    RETURN_WIN32(error ? error : ERROR_READ_FAULT);
  }
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  do {
    // The pattern also matches short (8.3) names; only the long name is checked.
    if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      continue;
    if (!IsProbeFileName(entry.cFileName) || !IsStaleProbe(entry.ftLastWriteTime, now)) continue;
    if (DeleteFileW((dir / entry.cFileName).c_str()) && removed) ++*removed;
  } while (FindNextFileW(find.get(), &entry));
  return S_OK;
}
CATCH_RETURN();

TestFileProblem TestFilePathProblem(std::wstring_view path) noexcept {
  if (path.empty()) return TestFileProblem::kEmpty;
  if (path.find(L'\0') != std::wstring_view::npos) return TestFileProblem::kBadName;
  // Windows takes '/' as a separator too, so "//./COM1" is a device path as well.
  const auto at = [path](size_t i) { return path[i] == L'/' ? L'\\' : path[i]; };
  if (path.size() >= 4 && at(0) == L'\\' && at(3) == L'\\' &&
      ((at(1) == L'\\' && (at(2) == L'.' || at(2) == L'?')) || (at(1) == L'?' && at(2) == L'?'))) {
    return TestFileProblem::kDevicePath;
  }
  return TestFileProblem::kNone;
}

TestFileProblem OpenedTestFileProblem(DWORD fileType, DWORD attributes) noexcept {
  if (fileType != FILE_TYPE_DISK) return TestFileProblem::kNotDiskFile;
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) return TestFileProblem::kDirectory;
  return TestFileProblem::kNone;
}

HRESULT ValidateTestFile(const std::filesystem::path& path, std::filesystem::path* resolved,
                         TestFileProblem* problem) noexcept try {
  resolved->clear();
  *problem = TestFilePathProblem(path.native());
  if (*problem == TestFileProblem::kEmpty || *problem == TestFileProblem::kBadName) {
    return E_INVALIDARG;
  }
  if (*problem == TestFileProblem::kDevicePath) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  std::error_code error;
  const std::filesystem::path full = std::filesystem::absolute(path, error);
  if (error) {
    *problem = TestFileProblem::kBadName;
    return HRESULT_FROM_WIN32(error.value());
  }
  // A reserved name ("NUL", "COM1") becomes a device path only now ("\\.\NUL").
  if (TestFilePathProblem(full.native()) != TestFileProblem::kNone) {
    *problem = TestFileProblem::kDevicePath;
    return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  }
  // Attributes only, sharing everything; links followed; folders open too (backup semantics),
  // so they can be told apart from missing files.
  wil::unique_hfile file(CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!file) {
    const DWORD code = GetLastError();
    const bool missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
                         code == ERROR_INVALID_NAME || code == ERROR_BAD_NETPATH ||
                         code == ERROR_CANT_RESOLVE_FILENAME;
    *problem = missing ? TestFileProblem::kNotFound : TestFileProblem::kUnreadable;
    return HRESULT_FROM_WIN32(code ? code : ERROR_OPEN_FAILED);
  }
  const DWORD type = GetFileType(file.get());
  FILE_BASIC_INFO basic = {};
  if (type == FILE_TYPE_DISK &&
      !GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof(basic))) {
    *problem = TestFileProblem::kUnreadable;
    return HRESULT_FROM_WIN32(GetLastError());
  }
  *problem = OpenedTestFileProblem(type, basic.FileAttributes);
  if (*problem == TestFileProblem::kNotDiskFile) return HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE);
  if (*problem == TestFileProblem::kDirectory) {
    return HRESULT_FROM_WIN32(ERROR_DIRECTORY_NOT_SUPPORTED);
  }
  file.reset();
  const HRESULT hr = Canonicalize(full, resolved);
  if (FAILED(hr)) {
    *problem = TestFileProblem::kUnreadable;
    resolved->clear();
    return hr;
  }
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
