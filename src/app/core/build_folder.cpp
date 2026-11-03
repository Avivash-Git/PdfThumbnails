#include "build_folder.h"

#include <bcrypt.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <utility>

#include "pe_info.h"
#include "project_config.h"

namespace pdft {
namespace {

// Files are hashed in pieces of this size.
constexpr DWORD kHashChunkBytes = 64 * 1024;

// The build id is this many bytes of the digest, as hex.
constexpr size_t kBuildIdBytes = 4;

bool IsHexDigit(wchar_t c) noexcept {
  return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// A file that exists and is not a folder.
bool IsFile(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The two DLL names a build folder of |is1x| holds. 1.x named them after its own product name,
// which never changes; 2.x builds use the names in project_config.h.
std::pair<std::wstring, std::wstring> DllNames(bool is1x) {
  if (is1x) {
    return {std::wstring(kLegacyRootName) + L".dll",
            std::wstring(kLegacyRootName) + L"-pdfium.dll"};
  }
  return {kHandlerDll, kPdfiumDll};
}

}  // namespace

// CNG's shared algorithm object: nothing to open or close but the hash.
Sha256Stream::~Sha256Stream() {
  if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
}

HRESULT Sha256Stream::Start() noexcept {
  RETURN_HR_IF(E_UNEXPECTED, hash_ != nullptr);
  BCRYPT_HASH_HANDLE hash = nullptr;
  const NTSTATUS status =
      BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0);
  RETURN_HR_IF(HRESULT_FROM_NT(status), !BCRYPT_SUCCESS(status));
  hash_ = hash;
  return S_OK;
}

HRESULT Sha256Stream::Add(const BYTE* bytes, size_t size) noexcept {
  RETURN_HR_IF(E_UNEXPECTED, hash_ == nullptr);
  while (size > 0) {
    const ULONG piece = static_cast<ULONG>(std::min<size_t>(size, kHashChunkBytes));
    // BCryptHashData does not write through the pointer; its parameter is not const.
    const NTSTATUS status =
        BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), const_cast<BYTE*>(bytes), piece, 0);
    RETURN_HR_IF(HRESULT_FROM_NT(status), !BCRYPT_SUCCESS(status));
    bytes += piece;
    size -= piece;
  }
  return S_OK;
}

HRESULT Sha256Stream::Finish(Sha256Digest* digest) noexcept {
  RETURN_HR_IF(E_UNEXPECTED, hash_ == nullptr);
  const NTSTATUS status = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest->data(),
                                           static_cast<ULONG>(digest->size()), 0);
  RETURN_HR_IF(HRESULT_FROM_NT(status), !BCRYPT_SUCCESS(status));
  return S_OK;
}

HRESULT Sha256Bytes(std::span<const BYTE> bytes, Sha256Digest* digest) noexcept {
  digest->fill(0);
  Sha256Stream hash;
  RETURN_IF_FAILED(hash.Start());
  RETURN_IF_FAILED(hash.Add(bytes.data(), bytes.size()));
  return hash.Finish(digest);
}

HRESULT Sha256File(const std::filesystem::path& file, Sha256Digest* digest) noexcept {
  digest->fill(0);
  wil::unique_hfile handle;
  const HRESULT opened = OpenFileForReading(file, handle.put());
  if (FAILED(opened)) return opened;
  return Sha256OpenFile(handle.get(), digest);
}

HRESULT Sha256OpenFile(HANDLE file, Sha256Digest* digest) noexcept try {
  digest->fill(0);
  LARGE_INTEGER origin = {};
  RETURN_IF_WIN32_BOOL_FALSE(SetFilePointerEx(file, origin, nullptr, FILE_BEGIN));
  Sha256Stream hash;
  RETURN_IF_FAILED(hash.Start());
  std::vector<BYTE> buffer(kHashChunkBytes);
  for (;;) {
    DWORD read = 0;
    RETURN_IF_WIN32_BOOL_FALSE(
        ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr));
    if (read == 0) break;
    RETURN_IF_FAILED(hash.Add(buffer.data(), read));
  }
  return hash.Finish(digest);
}
CATCH_RETURN();

std::string BuildIdHashInput(std::span<const BYTE, kSha256Bytes> dll,
                             std::span<const BYTE, kSha256Bytes> pdfium) {
  static constexpr char kUpperHex[] = "0123456789ABCDEF";
  std::string text;
  text.reserve(4 * kSha256Bytes);
  for (const std::span<const BYTE, kSha256Bytes> digest : {dll, pdfium}) {
    for (const BYTE b : digest) {
      text.push_back(kUpperHex[b >> 4]);
      text.push_back(kUpperHex[b & 0xF]);
    }
  }
  return text;
}

bool IsBuildVersion(std::wstring_view version) noexcept {
  constexpr size_t kMaxParts = 4;
  constexpr size_t kMaxDigits = 5;
  size_t parts = 1;
  size_t digits = 0;
  for (const wchar_t c : version) {
    if (c == L'.') {
      if (digits == 0 || ++parts > kMaxParts) return false;
      digits = 0;
    } else if (c >= L'0' && c <= L'9') {
      if (++digits > kMaxDigits) return false;
    } else {
      return false;
    }
  }
  return digits > 0;
}

std::wstring BuildFolderName(std::wstring_view version,
                             std::span<const BYTE, kSha256Bytes> idDigest) {
  static constexpr wchar_t kLowerHex[] = L"0123456789abcdef";
  std::wstring name(version);
  name.push_back(L'-');
  for (size_t i = 0; i < kBuildIdBytes; ++i) {
    name.push_back(kLowerHex[idDigest[i] >> 4]);
    name.push_back(kLowerHex[idDigest[i] & 0xF]);
  }
  return name;
}

HRESULT BuildIdFromHashes(std::wstring_view version, std::span<const BYTE, kSha256Bytes> dll,
                          std::span<const BYTE, kSha256Bytes> pdfium, std::wstring* name) noexcept
    try {
  name->clear();
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), !IsBuildVersion(version));
  const std::string input = BuildIdHashInput(dll, pdfium);
  Sha256Digest id = {};
  RETURN_IF_FAILED(Sha256Bytes(
      std::span<const BYTE>(reinterpret_cast<const BYTE*>(input.data()), input.size()), &id));
  *name = BuildFolderName(version, id);
  return S_OK;
}
CATCH_RETURN();

HRESULT ComputeBuildId(const std::filesystem::path& dll, const std::filesystem::path& pdfium,
                       std::wstring* name, std::wstring_view version) noexcept try {
  name->clear();
  std::wstring fileVersion(version);
  if (fileVersion.empty()) RETURN_IF_FAILED(ReadFileVersionString(dll, &fileVersion));
  Sha256Digest dllDigest = {};
  Sha256Digest pdfiumDigest = {};
  RETURN_IF_FAILED(Sha256File(dll, &dllDigest));
  RETURN_IF_FAILED(Sha256File(pdfium, &pdfiumDigest));
  return BuildIdFromHashes(fileVersion, dllDigest, pdfiumDigest, name);
}
CATCH_RETURN();

std::optional<BuildFolderNameParts> ParseBuildFolderName(std::wstring_view name) {
  const size_t dash = name.rfind(L'-');
  if (dash == std::wstring_view::npos) return std::nullopt;
  const std::wstring_view version = name.substr(0, dash);
  const std::wstring_view id = name.substr(dash + 1);
  if (id.size() != 2 * kBuildIdBytes || !std::all_of(id.begin(), id.end(), IsHexDigit) ||
      !IsBuildVersion(version)) {
    return std::nullopt;
  }
  return BuildFolderNameParts{std::wstring(version), std::wstring(id)};
}

int CompareFileVersions(std::wstring_view a, std::wstring_view b) noexcept {
  // The next number of |text| from |at| on, and |at| moved past its dot. Saturates instead of
  // overflowing; anything that is not a digit ends the number.
  const auto next = [](std::wstring_view text, size_t* at) {
    unsigned long long value = 0;
    while (*at < text.size() && text[*at] != L'.') {
      const wchar_t c = text[*at];
      if (c >= L'0' && c <= L'9' && value < 1'000'000'000ULL) value = value * 10 + (c - L'0');
      ++*at;
    }
    if (*at < text.size()) ++*at;  // the dot
    return value;
  };
  size_t atA = 0;
  size_t atB = 0;
  while (atA < a.size() || atB < b.size()) {
    const unsigned long long partA = next(a, &atA);
    const unsigned long long partB = next(b, &atB);
    if (partA != partB) return partA < partB ? -1 : 1;
  }
  return 0;
}

HRESULT ListBuildFolders(const std::filesystem::path& appDir, std::vector<BuildFolder>* folders,
                         std::vector<std::filesystem::path>* skipped) noexcept try {
  folders->clear();
  if (skipped) skipped->clear();
  const DWORD rootAttributes = GetFileAttributesW(appDir.c_str());
  if (rootAttributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_OK;
    RETURN_WIN32(error);
  }
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_DIRECTORY), !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY));
  if (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    // The app folder itself leads elsewhere: nothing in it is ours to report (or, later, delete).
    if (skipped) skipped->push_back(appDir);
    return S_OK;
  }

  WIN32_FIND_DATAW entry = {};
  wil::unique_hfind find(FindFirstFileExW((appDir / L"*").c_str(), FindExInfoBasic, &entry,
                                          FindExSearchNameMatch, nullptr,
                                          FIND_FIRST_EX_LARGE_FETCH));
  if (!find) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_OK;
    RETURN_WIN32(error);
  }
  do {
    const std::wstring_view name = entry.cFileName;
    if (name == L"." || name == L".." || !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      continue;
    }
    BuildFolder folder;
    folder.dir = appDir / name;
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      if (skipped) skipped->push_back(folder.dir);
      continue;
    }
    folder.name = name;
    if (const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(name)) {
      folder.version = parts->version;
      folder.is1x = folder.version == L"1" || folder.version.starts_with(L"1.");
    }
    const auto [dll, pdfium] = DllNames(folder.is1x);
    folder.complete = IsFile(folder.dir / dll) && IsFile(folder.dir / pdfium);
    folders->push_back(std::move(folder));
  } while (FindNextFileW(find.get(), &entry));
  const DWORD error = GetLastError();
  RETURN_HR_IF(HRESULT_FROM_WIN32(error), error != ERROR_NO_MORE_FILES);

  std::sort(folders->begin(), folders->end(), [](const BuildFolder& a, const BuildFolder& b) {
    return CompareStringOrdinal(a.name.c_str(), static_cast<int>(a.name.size()), b.name.c_str(),
                                static_cast<int>(b.name.size()), TRUE) == CSTR_LESS_THAN;
  });
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
