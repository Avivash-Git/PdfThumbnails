// What the app reads from a DLL without running any of it: the machine type in its PE headers
// and its version string.
//
// The DLL a COM registration names is untrusted input (anyone in the session can point the
// per-user class anywhere): the parsers check every offset and size against the bytes actually
// read and reject what does not add up with a specific HRESULT, never by reading past the end.
//
// Architectures, for what the check engine asks ("can Windows' native thumbnail process load
// this DLL?"):
// - The header's Machine field is IMAGE_FILE_MACHINE_AMD64 for x64 and also for ARM64EC images,
//   and IMAGE_FILE_MACHINE_ARM64 for ARM64 and also for ARM64X images (the hybrid form that
//   carries both). The header alone cannot tell those apart, and does not need to: an x64 or
//   ARM64EC DLL loads only into x64-compatible processes, never into a native ARM64 one, and an
//   ARM64 or ARM64X DLL always loads into a native ARM64 process. So the header's answer is the
//   right one for the native surrogate on either kind of Windows.
// - IMAGE_FILE_MACHINE_ARM64EC (0xA641) and IMAGE_FILE_MACHINE_ARM64X (0xA64E) appear in object
//   files, not in the headers of linked images. Should a header carry one anyway, ARM64X counts
//   as ARM64 (it holds native ARM64 code) and ARM64EC as "other" (it cannot load into a native
//   ARM64 process).

#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "exit_code.h"

namespace pdft {

// How much of a file ReadPeMachine reads: the DOS header points at the PE header, which linkers
// place within the first few hundred bytes. A file whose PE header starts beyond this is refused
// as malformed rather than read further.
inline constexpr size_t kPeHeadBytes = 64 * 1024;

// The Machine field of a PE file whose first bytes are |head| and whose total size is |fileSize|
// (pure). HRESULT_FROM_WIN32(ERROR_HANDLE_EOF) when the file ends before the headers do
// (truncated); HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT) when the headers are wrong: no "MZ", a
// negative or too distant PE header offset, no "PE\0\0", or an optional header that is too small
// or neither PE32 nor PE32+.
[[nodiscard]] HRESULT ParsePeMachine(std::span<const BYTE> head, std::uint64_t fileSize,
                                     USHORT* machine) noexcept;

// Opens an existing file for reading, letting others read, write and delete it meanwhile, and
// checks that it is a file on disk: a path from the registry could name a pipe or a device,
// where a read might wait forever (HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE) then). A path that
// does not exist fails with its own error, which callers treat as an answer.
[[nodiscard]] HRESULT OpenFileForReading(const std::filesystem::path& file,
                                         HANDLE* handle) noexcept;

// Reads the first kPeHeadBytes of |file| (OpenFileForReading) and parses them like
// ParsePeMachine.
[[nodiscard]] HRESULT ReadPeMachine(const std::filesystem::path& file, USHORT* machine) noexcept;

// IMAGE_FILE_MACHINE_* to Arch, as described at the top (pure).
Arch MachineToArch(USHORT machine) noexcept;

// The file version in a VS_VERSIONINFO resource |resource| (pure): the FileVersion string of the
// 040904b0 string table (U.S. English, Unicode), else of the first string table that has one,
// else the fixed file version as "a.b.c.d". HRESULT_FROM_WIN32(ERROR_INVALID_DATA) when the
// blocks do not fit inside each other or the resource; HRESULT_FROM_WIN32(ERROR_NOT_FOUND) when
// it holds no version at all. May allocate; failures to do so come back as E_OUTOFMEMORY.
[[nodiscard]] HRESULT ParseVersionResource(std::span<const BYTE> resource,
                                           std::wstring* fileVersion) noexcept;

// The file version of |file| ("1.1.0.0", "2.0.0.0"), read like ParseVersionResource from its
// version resource. The file is mapped as a resource-only image (never as code, so nothing in it
// runs and Code Integrity does not judge it), or as plain data where Windows refuses the image
// mapping (another architecture's DLL). HRESULT_FROM_WIN32(ERROR_RESOURCE_TYPE_NOT_FOUND) or
// ERROR_RESOURCE_NAME_NOT_FOUND when it has no version resource.
[[nodiscard]] HRESULT ReadFileVersionString(const std::filesystem::path& file,
                                            std::wstring* version) noexcept;

}  // namespace pdft
