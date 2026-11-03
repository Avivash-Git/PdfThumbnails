#include "pe_info.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include "strings.h"

namespace pdft {
namespace {

// Machine values that occur in object files; the Windows SDK defines them only in newer
// versions, so they are spelled out here.
constexpr USHORT kMachineArm64Ec = 0xA641;
constexpr USHORT kMachineArm64X = 0xA64E;

constexpr HRESULT kTruncated = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
constexpr HRESULT kBadFormat = HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
constexpr HRESULT kBadResource = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

// The resource ID and type of a version resource (VS_VERSION_INFO, RT_VERSION).
constexpr WORD kVersionResourceId = 1;
constexpr DWORD kFixedFileInfoSignature = 0xFEEF04BD;

// A copy of the T at |offset| in |bytes|; the caller has checked that it fits. Copies instead of
// casting, because nothing aligns the file's bytes.
template <typename T>
T ReadAt(std::span<const BYTE> bytes, size_t offset) noexcept {
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

constexpr size_t Align4(size_t offset) noexcept { return (offset + 3) & ~size_t{3}; }

// One block of a VS_VERSIONINFO resource: { WORD wLength; WORD wValueLength; WORD wType;
// WCHAR szKey[]; padding; value; padding; children }. Offsets are into the whole resource.
struct VersionBlock {
  size_t end = 0;         // one past the block's last byte
  std::wstring key;       // szKey
  size_t valueStart = 0;  // where the value begins
  size_t valueBytes = 0;  // its size, cut to the block's end for text values
  size_t childrenStart = 0;
};

// Keys in a version resource are short ("VS_VERSION_INFO", "StringFileInfo", "040904b0",
// "FileVersion", ...); anything much longer is not one.
constexpr size_t kMaxKeyChars = 64;

enum class BlockRead { kOk, kEnd, kBad };

// Reads the block at |offset|, which must end by |limit|. kEnd for a zero length (some resource
// writers pad the list of children with one), kBad when it does not fit.
BlockRead ReadVersionBlock(std::span<const BYTE> data, size_t offset, size_t limit,
                           VersionBlock* block) {
  constexpr size_t kHeader = 3 * sizeof(WORD);
  if (offset > limit || limit - offset < kHeader) return BlockRead::kBad;
  const WORD length = ReadAt<WORD>(data, offset);
  const WORD valueLength = ReadAt<WORD>(data, offset + sizeof(WORD));
  const WORD type = ReadAt<WORD>(data, offset + 2 * sizeof(WORD));
  if (length == 0) return BlockRead::kEnd;
  if (length < kHeader || length > limit - offset) return BlockRead::kBad;
  block->end = offset + length;
  block->key.clear();
  size_t at = offset + kHeader;
  for (;;) {
    if (block->end - at < sizeof(wchar_t)) return BlockRead::kBad;  // no terminating null
    const wchar_t c = ReadAt<wchar_t>(data, at);
    at += sizeof(wchar_t);
    if (c == L'\0') break;
    if (block->key.size() == kMaxKeyChars) return BlockRead::kBad;
    block->key.push_back(c);
  }
  block->valueStart = std::min(Align4(at), block->end);
  const size_t room = block->end - block->valueStart;
  if (type == 1) {
    // Text: wValueLength counts characters, though some writers count bytes; either way the
    // value cannot reach past its block.
    block->valueBytes = std::min<size_t>(size_t{valueLength} * sizeof(wchar_t), room);
  } else {
    if (valueLength > room) return BlockRead::kBad;
    block->valueBytes = valueLength;
  }
  block->childrenStart = std::min(Align4(block->valueStart + block->valueBytes), block->end);
  return BlockRead::kOk;
}

// Calls |visit| for each child block of |parent|; false when one is malformed or |visit| says so.
template <typename Visit>
bool ForEachChild(std::span<const BYTE> data, const VersionBlock& parent, Visit visit) {
  size_t offset = parent.childrenStart;
  // Fewer bytes than a block header are padding at the end of the parent.
  while (offset < parent.end && parent.end - offset >= 3 * sizeof(WORD)) {
    VersionBlock child;
    const BlockRead read = ReadVersionBlock(data, offset, parent.end, &child);
    if (read == BlockRead::kEnd) return true;
    if (read == BlockRead::kBad) return false;
    if (!visit(child)) return false;
    offset = Align4(child.end);
  }
  return true;
}

// A text value up to its first null character.
std::wstring TextValue(std::span<const BYTE> data, const VersionBlock& block) {
  std::wstring text(block.valueBytes / sizeof(wchar_t), L'\0');
  if (!text.empty()) {
    std::memcpy(text.data(), data.data() + block.valueStart, text.size() * sizeof(wchar_t));
  }
  const size_t end = text.find(L'\0');
  if (end != std::wstring::npos) text.resize(end);
  return text;
}

}  // namespace

HRESULT ParsePeMachine(std::span<const BYTE> head, std::uint64_t fileSize,
                       USHORT* machine) noexcept {
  *machine = IMAGE_FILE_MACHINE_UNKNOWN;
  // Whether the bytes up to |end| were read. Beyond the end of the file the file is truncated;
  // within the file but beyond what was read, the headers are not where headers can be.
  const auto have = [&](std::uint64_t end) -> HRESULT {
    if (end <= head.size()) return S_OK;
    return end > fileSize ? kTruncated : kBadFormat;
  };
  HRESULT hr = have(sizeof(IMAGE_DOS_HEADER));
  if (FAILED(hr)) return hr;
  const IMAGE_DOS_HEADER dos = ReadAt<IMAGE_DOS_HEADER>(head, 0);
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return kBadFormat;

  const std::uint64_t signatureAt = static_cast<std::uint64_t>(dos.e_lfanew);
  const std::uint64_t fileHeaderAt = signatureAt + sizeof(DWORD);
  const std::uint64_t optionalAt = fileHeaderAt + sizeof(IMAGE_FILE_HEADER);
  hr = have(optionalAt + sizeof(WORD));  // up to the optional header's Magic
  if (FAILED(hr)) return hr;
  if (ReadAt<DWORD>(head, static_cast<size_t>(signatureAt)) != IMAGE_NT_SIGNATURE)
    return kBadFormat;
  const IMAGE_FILE_HEADER fileHeader =
      ReadAt<IMAGE_FILE_HEADER>(head, static_cast<size_t>(fileHeaderAt));
  if (fileHeader.SizeOfOptionalHeader < sizeof(WORD)) return kBadFormat;
  const WORD magic = ReadAt<WORD>(head, static_cast<size_t>(optionalAt));
  if (magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC && magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    return kBadFormat;
  }
  // The loader needs the whole optional header; a file that ends inside it is truncated.
  if (optionalAt + fileHeader.SizeOfOptionalHeader > fileSize) return kTruncated;
  *machine = fileHeader.Machine;
  return S_OK;
}

HRESULT OpenFileForReading(const std::filesystem::path& file, HANDLE* handle) noexcept {
  *handle = INVALID_HANDLE_VALUE;
  // A path from the registry could name a pipe that someone else serves: it may learn who opened
  // it, but never act as them (identification only).
  wil::unique_hfile opened(CreateFileW(
      file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
      nullptr));
  if (!opened) {
    // A missing file is an answer for the callers, not a failure worth logging.
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }
  if (GetFileType(opened.get()) != FILE_TYPE_DISK) return HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE);
  *handle = opened.release();
  return S_OK;
}

HRESULT ReadPeMachine(const std::filesystem::path& file, USHORT* machine) noexcept try {
  *machine = IMAGE_FILE_MACHINE_UNKNOWN;
  wil::unique_hfile handle;
  const HRESULT opened = OpenFileForReading(file, handle.put());
  if (FAILED(opened)) return opened;
  LARGE_INTEGER size = {};
  RETURN_IF_WIN32_BOOL_FALSE(GetFileSizeEx(handle.get(), &size));
  const std::uint64_t fileSize = static_cast<std::uint64_t>(size.QuadPart);
  std::vector<BYTE> head(static_cast<size_t>(std::min<std::uint64_t>(fileSize, kPeHeadBytes)));
  size_t got = 0;
  while (got < head.size()) {
    DWORD read = 0;
    RETURN_IF_WIN32_BOOL_FALSE(ReadFile(handle.get(), head.data() + got,
                                        static_cast<DWORD>(head.size() - got), &read, nullptr));
    if (read == 0) break;  // the file shrank meanwhile: parse what there is
    got += read;
  }
  return ParsePeMachine(std::span<const BYTE>(head.data(), got), fileSize, machine);
}
CATCH_RETURN();

Arch MachineToArch(USHORT machine) noexcept {
  switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64:
      return Arch::kX64;
    case IMAGE_FILE_MACHINE_ARM64:
    case kMachineArm64X:
      return Arch::kArm64;
    case kMachineArm64Ec:
    default:
      return Arch::kOther;
  }
}

HRESULT ParseVersionResource(std::span<const BYTE> resource, std::wstring* fileVersion) noexcept
    try {
  fileVersion->clear();
  VersionBlock root;
  if (ReadVersionBlock(resource, 0, resource.size(), &root) != BlockRead::kOk ||
      root.key != L"VS_VERSION_INFO") {
    return kBadResource;
  }

  std::optional<std::wstring> fixed;
  if (root.valueBytes >= sizeof(VS_FIXEDFILEINFO)) {
    const VS_FIXEDFILEINFO info = ReadAt<VS_FIXEDFILEINFO>(resource, root.valueStart);
    if (info.dwSignature == kFixedFileInfoSignature) {
      wchar_t text[32] = {};
      swprintf_s(text, L"%u.%u.%u.%u", HIWORD(info.dwFileVersionMS), LOWORD(info.dwFileVersionMS),
                 HIWORD(info.dwFileVersionLS), LOWORD(info.dwFileVersionLS));
      fixed = text;
    }
  }

  std::optional<std::wstring> preferred;  // from the 040904b0 table
  std::optional<std::wstring> first;      // from the first table that has one
  const bool wellFormed = ForEachChild(resource, root, [&](const VersionBlock& info) {
    if (info.key != L"StringFileInfo") return true;  // VarFileInfo and the like
    return ForEachChild(resource, info, [&](const VersionBlock& table) {
      return ForEachChild(resource, table, [&](const VersionBlock& string) {
        if (!IEquals(string.key, L"FileVersion")) return true;
        std::wstring text = TextValue(resource, string);
        if (text.empty()) return true;
        if (!preferred && IEquals(table.key, L"040904b0")) preferred = text;
        if (!first) first = std::move(text);
        return true;
      });
    });
  });
  if (!wellFormed) return kBadResource;
  if (preferred) {
    *fileVersion = std::move(*preferred);
  } else if (first) {
    *fileVersion = std::move(*first);
  } else if (fixed) {
    *fileVersion = std::move(*fixed);
  } else {
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT ReadFileVersionString(const std::filesystem::path& file, std::wstring* version) noexcept
    try {
  version->clear();
  {
    // A file on disk, not a pipe or a device (OpenFileForReading says why not).
    wil::unique_hfile check;
    const HRESULT opened = OpenFileForReading(file, check.put());
    if (FAILED(opened)) return opened;
  }
  // As a resource-only image first: Windows then finds resources through the image's own layout.
  // Windows refuses image mappings of other architectures' DLLs on some systems; plain data
  // mapping works for any file.
  HMODULE module = LoadLibraryExW(file.c_str(), nullptr,
                                  LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module && GetLastError() == ERROR_BAD_EXE_FORMAT) {
    module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  }
  if (!module) {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }
  wil::unique_hmodule owner(module);
  HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(kVersionResourceId), RT_VERSION);
  if (!info) {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : HRESULT_FROM_WIN32(ERROR_RESOURCE_TYPE_NOT_FOUND);
  }
  const DWORD size = SizeofResource(module, info);
  RETURN_LAST_ERROR_IF(size == 0);
  HGLOBAL loaded = LoadResource(module, info);
  RETURN_LAST_ERROR_IF_NULL(loaded);
  const void* bytes = LockResource(loaded);
  RETURN_HR_IF_NULL(kBadResource, bytes);
  return ParseVersionResource(std::span<const BYTE>(static_cast<const BYTE*>(bytes), size),
                              version);
}
CATCH_RETURN();

}  // namespace pdft
