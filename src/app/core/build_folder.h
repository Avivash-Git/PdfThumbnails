// Build folders. Each build of the handler is installed into its own folder,
// <install root>\app\<FileVersion>-<build id>, because Explorer's thumbnail process keeps the
// DLLs it loaded open: a new build never overwrites an old one in place, and an old folder is
// removed once nothing holds it.
//
// The build id comes from the final bytes of the two DLLs, with the formula 1.x used (kept, so
// that folders of every version are named the same way):
//   build id = lower-hex(first 4 bytes of SHA-256(ASCII(UPPER-HEX(SHA-256(dll)) +
//                                                       UPPER-HEX(SHA-256(pdfium)))))
// cmake/BuildId.cmake computes the same name for builds and installers; a test compares the two.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pdft {

inline constexpr size_t kSha256Bytes = 32;
using Sha256Digest = std::array<BYTE, kSha256Bytes>;

// A SHA-256 computed piece by piece, for data that streams past (a file being copied). Start
// once, Add any number of times, Finish once.
class Sha256Stream {
 public:
  Sha256Stream() = default;
  Sha256Stream(const Sha256Stream&) = delete;
  Sha256Stream& operator=(const Sha256Stream&) = delete;
  ~Sha256Stream();

  [[nodiscard]] HRESULT Start() noexcept;
  [[nodiscard]] HRESULT Add(const BYTE* bytes, size_t size) noexcept;
  [[nodiscard]] HRESULT Finish(Sha256Digest* digest) noexcept;

 private:
  void* hash_ = nullptr;  // BCRYPT_HASH_HANDLE
};

// SHA-256 of |bytes| and of a whole file (read in chunks; a pipe or device is refused like in
// OpenFileForReading, pe_info.h).
[[nodiscard]] HRESULT Sha256Bytes(std::span<const BYTE> bytes, Sha256Digest* digest) noexcept;
[[nodiscard]] HRESULT Sha256File(const std::filesystem::path& file, Sha256Digest* digest) noexcept;

// The text the build id hashes: the two digests in upper-case hex, DLL first (pure; 128
// characters). May throw std::bad_alloc.
std::string BuildIdHashInput(std::span<const BYTE, kSha256Bytes> dll,
                             std::span<const BYTE, kSha256Bytes> pdfium);

// A FileVersion that may name a build folder (pure): one to four dot-separated numbers of one to
// five digits ("1.1.0.0", "2.0.0.0"). The version comes from the DLL, which may be anyone's; a
// folder name built from it must never contain a path separator or "..".
bool IsBuildVersion(std::wstring_view version) noexcept;

// "<version>-<first 4 bytes of |idDigest| in lower-case hex>" (pure). May throw std::bad_alloc.
std::wstring BuildFolderName(std::wstring_view version,
                             std::span<const BYTE, kSha256Bytes> idDigest);

// The folder name for a build whose DLLs have the digests |dll| and |pdfium|. Fails with
// HRESULT_FROM_WIN32(ERROR_INVALID_DATA) when |version| is not IsBuildVersion.
[[nodiscard]] HRESULT BuildIdFromHashes(std::wstring_view version,
                                        std::span<const BYTE, kSha256Bytes> dll,
                                        std::span<const BYTE, kSha256Bytes> pdfium,
                                        std::wstring* name) noexcept;

// The folder name for the DLL files |dll| and |pdfium|, with |version| or, when it is empty,
// the handler's own FileVersion string (ReadFileVersionString), like cmake/BuildId.cmake.
[[nodiscard]] HRESULT ComputeBuildId(const std::filesystem::path& dll,
                                     const std::filesystem::path& pdfium, std::wstring* name,
                                     std::wstring_view version = {}) noexcept;

// A folder name split into its version and build id (pure): std::nullopt unless it is
// "<IsBuildVersion>-<8 hex digits>". May throw std::bad_alloc.
struct BuildFolderNameParts {
  std::wstring version;
  std::wstring id;
};
std::optional<BuildFolderNameParts> ParseBuildFolderName(std::wstring_view name);

// Orders two versions number by number, missing parts counting as 0, so "1.10" comes after
// "1.9" and "2.0" equals "2.0.0.0" (pure). Negative, 0 or positive like a comparison function.
// A part that is not a number counts as 0.
int CompareFileVersions(std::wstring_view a, std::wstring_view b) noexcept;

struct BuildFolder {
  std::filesystem::path dir;
  std::wstring name;      // the folder's name
  std::wstring version;   // from the name; empty when the name is not a build folder name
  bool is1x = false;      // the version is 1.something: a 1.x install made this folder
  bool complete = false;  // both DLLs are there, as files
};

// The folders in |appDir|, sorted by name ignoring case. Anything else in there is ignored;
// folders that are links or junctions (reparse points) are never followed: they are left out and
// listed in |skipped| (optional) instead. A missing |appDir| gives an empty list.
[[nodiscard]] HRESULT ListBuildFolders(
    const std::filesystem::path& appDir, std::vector<BuildFolder>* folders,
    std::vector<std::filesystem::path>* skipped = nullptr) noexcept;

}  // namespace pdft
