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
#include <vector>

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

// How much of a folder's listing one call reads.
constexpr size_t kListingBytes = 64 * 1024;

// What the caller and the worker thread share. The caller and the thread each hold a reference
// (std::shared_ptr), so the block lives until the later of the two lets go: after a timeout the
// caller returns and drops its reference, and the worker, still inside the shell, keeps writing
// into memory that stays valid until it ends. The caller reads |result| only after |finished| is
// set, which orders the worker's writes before the read.
struct LiveTestJob {
  std::wstring file;
  UINT size = 0;
  bool ownsFile = false;  // a probe written for this test: the worker deletes it when done
  // Where that probe really is (FinalPath), checked before it is deleted.
  std::wstring finalPath;
  LiveTest result;
  wil::unique_event_nothrow finished;
};

// The path Windows resolved |handle| to, in the volume's device form (\Device\HarddiskVolume3\...):
// where any junction or link on the way really led, with no drive letter to look up.
HRESULT FinalPath(HANDLE handle, std::wstring* path) noexcept try {
  path->clear();
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    RETURN_LAST_ERROR_IF(length == 0);
    if (length < buffer.size()) {
      buffer.resize(length);
      *path = std::move(buffer);
      return S_OK;
    }
    // Too small: |length| is the size needed, terminating null included.
    buffer.resize(length);
  }
}
CATCH_RETURN();

// The open handle is a regular file on disk, opened as itself: not a link or another reparse
// point, not a folder, not a pipe or a device.
bool IsRegularFile(HANDLE file) noexcept {
  if (GetFileType(file) != FILE_TYPE_DISK) return false;
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  return GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) &&
         !(tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

// Opens |path| to delete it, as itself (a link at the end is not followed), when it is a regular
// file, and returns where it really is in |finalPath|. An empty handle otherwise (missing, in use,
// a link, a folder).
wil::unique_hfile OpenFileToDelete(const std::wstring& path, std::wstring* finalPath) noexcept {
  finalPath->clear();
  wil::unique_hfile file(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr));
  if (!file || !IsRegularFile(file.get()) || FAILED(FinalPath(file.get(), finalPath))) {
    return wil::unique_hfile();
  }
  return file;
}

// Deletes the file open as |file| (with DELETE access) through that handle, so its path is never
// looked up again: the name goes at once where the file system can (POSIX semantics), otherwise
// when the last handle closes.
HRESULT DeleteThroughHandle(HANDLE file) noexcept {
  FILE_DISPOSITION_INFO_EX dispositionEx = {};
  dispositionEx.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
  if (SetFileInformationByHandle(file, FileDispositionInfoEx, &dispositionEx,
                                 sizeof(dispositionEx))) {
    return S_OK;
  }
  const DWORD error = GetLastError();
  // File systems without the newer form (FAT, older NTFS drivers) take the classic one.
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
      error != ERROR_INVALID_FUNCTION) {
    return HRESULT_FROM_WIN32(error);
  }
  FILE_DISPOSITION_INFO disposition = {};
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
    const DWORD classic = GetLastError();
    return HRESULT_FROM_WIN32(classic ? classic : ERROR_ACCESS_DENIED);
  }
  return S_OK;
}

// Deletes the probe this process wrote at |path| (|finalPath| from WriteProbe), only while the
// name still leads to that very file: never a link, nor what something put in its place or
// redirected the folder to. Best effort.
void DeleteOwnProbe(const std::wstring& path, const std::wstring& finalPath) noexcept {
  std::wstring now;
  const wil::unique_hfile file = OpenFileToDelete(path, &now);
  if (file && IEquals(now, finalPath)) (void)DeleteThroughHandle(file.get());
}

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
  if (job->ownsFile) DeleteOwnProbe(job->file, job->finalPath);
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

FILETIME ToFileTime(const LARGE_INTEGER& time) noexcept {
  FILETIME result;
  result.dwLowDateTime = time.LowPart;
  result.dwHighDateTime = static_cast<DWORD>(time.HighPart);
  return result;
}

// Writes a new probe into %TEMP% (WriteProbePdf), and returns in |finalPath| where it really is
// (FinalPath), which DeleteOwnProbe checks.
HRESULT WriteProbe(std::filesystem::path* path, std::wstring* finalPath) noexcept try {
  path->clear();
  finalPath->clear();
  std::filesystem::path dir;
  RETURN_IF_FAILED(TempDirectory(&dir));
  GUID id;
  RETURN_IF_FAILED(CoCreateGuid(&id));
  std::filesystem::path probe = dir / ProbeFileName(id);
  const std::string bytes = ProbePdfBytes();
  // CREATE_NEW: never over a file (or a link) that is already there. A probe that is not written
  // whole goes again through this handle, never by its path.
  wil::unique_hfile file(CreateFileW(probe.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
  RETURN_LAST_ERROR_IF(!file);
  auto discard = wil::scope_exit([&]() noexcept { (void)DeleteThroughHandle(file.get()); });
  DWORD written = 0;
  if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
      written != bytes.size()) {
    const DWORD error = GetLastError();
    RETURN_HR(HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT));
  }
  RETURN_IF_FAILED(FinalPath(file.get(), finalPath));
  discard.release();
  *path = std::move(probe);
  return S_OK;
}
CATCH_RETURN();

// Deletes the probe |name| in the folder held open for a sweep (|dir|, whose final path is
// |folderPath|) when it is stale at |now|: opened as itself, and deleted through its handle only
// when it is a regular file in that very folder, never what a link or a folder redirected in the
// meantime leads to. S_OK when it went, S_FALSE when it stays.
HRESULT DeleteStaleProbe(const std::filesystem::path& dir, std::wstring_view name,
                         std::wstring_view folderPath, const FILETIME& now) noexcept try {
  std::wstring finalPath;
  const wil::unique_hfile file = OpenFileToDelete((dir / name).native(), &finalPath);
  if (!file) return S_FALSE;
  const std::wstring_view real = finalPath;
  const size_t separator = real.rfind(L'\\');
  if (separator == std::wstring_view::npos || !IEquals(real.substr(0, separator), folderPath) ||
      !IEquals(real.substr(separator + 1), name)) {
    return S_FALSE;
  }
  FILE_BASIC_INFO basic = {};
  if (!GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof(basic)) ||
      !IsStaleProbe(ToFileTime(basic.LastWriteTime), now)) {
    return S_FALSE;
  }
  return SUCCEEDED(DeleteThroughHandle(file.get())) ? S_OK : S_FALSE;
}
CATCH_RETURN();

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
    RETURN_IF_FAILED(WriteProbe(&probe, &job->finalPath));
    job->file = probe.native();
    job->ownsFile = true;
  }
  auto handoff = std::make_unique<std::shared_ptr<LiveTestJob>>(job);
  wil::unique_handle thread(CreateThread(nullptr, 0, LiveTestThread, handoff.get(), 0, nullptr));
  if (!thread) {
    const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    if (job->ownsFile) DeleteOwnProbe(job->file, job->finalPath);
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

HRESULT WriteProbePdf(std::filesystem::path* path) noexcept {
  std::wstring finalPath;
  return WriteProbe(path, &finalPath);
}

bool IsStaleProbe(const FILETIME& written, const FILETIME& now) noexcept {
  const ULONGLONG at = Ticks(written);
  const ULONGLONG current = Ticks(now);
  return at < current && current - at > kStaleProbeAge100ns;
}

HRESULT SweepStaleProbes(const std::filesystem::path& directory, int* removed) noexcept try {
  if (removed) *removed = 0;
  std::filesystem::path dir = directory;
  if (dir.empty()) RETURN_IF_FAILED(TempDirectory(&dir));
  // The folder itself, never what a junction or link in its place leads to. It stays open: the
  // probes are listed from this handle, and each one deleted must lie in this very folder.
  wil::unique_hfile folder(
      CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!folder) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_OK;
    RETURN_WIN32(error ? error : ERROR_OPEN_FAILED);
  }
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  RETURN_IF_WIN32_BOOL_FALSE(
      GetFileInformationByHandleEx(folder.get(), FileAttributeTagInfo, &tag, sizeof(tag)));
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      IsReparseTagNameSurrogate(tag.ReparseTag)) {
    return HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  }
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_DIRECTORY),
               !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY));
  std::wstring folderPath;
  RETURN_IF_FAILED(FinalPath(folder.get(), &folderPath));
  if (folderPath.ends_with(L'\\')) folderPath.pop_back();  // a volume's root

  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  // FILE_FULL_DIR_INFO holds 64-bit fields, so the buffer is aligned for them.
  std::vector<ULONGLONG> buffer(kListingBytes / sizeof(ULONGLONG));
  FILE_INFO_BY_HANDLE_CLASS listing = FileFullDirectoryRestartInfo;
  for (;;) {
    if (!GetFileInformationByHandleEx(folder.get(), listing, buffer.data(),
                                      static_cast<DWORD>(buffer.size() * sizeof(ULONGLONG)))) {
      const DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) return S_OK;
      RETURN_WIN32(error ? error : ERROR_READ_FAULT);
    }
    listing = FileFullDirectoryInfo;
    const BYTE* at = reinterpret_cast<const BYTE*>(buffer.data());
    for (;;) {
      const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(at);
      const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      // The long name only: a listing by handle has no short (8.3) names to match.
      if (!(entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
          IsProbeFileName(name) && IsStaleProbe(ToFileTime(entry->LastWriteTime), now) &&
          DeleteStaleProbe(dir, name, folderPath, now) == S_OK && removed) {
        ++*removed;
      }
      if (entry->NextEntryOffset == 0) break;
      at += entry->NextEntryOffset;
    }
  }
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
