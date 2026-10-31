// PE header and version-resource parsing on synthetic bytes (pe_info.h): well-formed files, and
// truncated or malformed ones, which must be refused with the documented HRESULT and never read
// past the bytes given.

#include <windows.h>

#include "pe_info.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace pdft {
namespace {

constexpr HRESULT kTruncated = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
constexpr HRESULT kBadFormat = HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
constexpr HRESULT kBadResource = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

template <typename T>
void Put(std::vector<BYTE>& bytes, size_t offset, const T& value) {
  if (bytes.size() < offset + sizeof(T)) bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

struct PeShape {
  USHORT machine = IMAGE_FILE_MACHINE_AMD64;
  LONG peOffset = 0x80;
  WORD optionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  WORD optionalSize = sizeof(IMAGE_OPTIONAL_HEADER64);
  size_t fileSize = 0x400;
  WORD dosMagic = IMAGE_DOS_SIGNATURE;
  DWORD signature = IMAGE_NT_SIGNATURE;
};

// A file with just the headers ParsePeMachine looks at, padded with zeros to |fileSize|.
std::vector<BYTE> MakePe(const PeShape& shape) {
  std::vector<BYTE> bytes(shape.fileSize, 0);
  IMAGE_DOS_HEADER dos = {};
  dos.e_magic = shape.dosMagic;
  dos.e_lfanew = shape.peOffset;
  Put(bytes, 0, dos);
  const size_t pe = static_cast<size_t>(shape.peOffset);
  Put(bytes, pe, shape.signature);
  IMAGE_FILE_HEADER header = {};
  header.Machine = shape.machine;
  header.SizeOfOptionalHeader = shape.optionalSize;
  header.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL;
  Put(bytes, pe + sizeof(DWORD), header);
  Put(bytes, pe + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER), shape.optionalMagic);
  bytes.resize(shape.fileSize);
  return bytes;
}

HRESULT Parse(const std::vector<BYTE>& bytes, USHORT* machine) {
  return ParsePeMachine(bytes, bytes.size(), machine);
}

TEST(PeMachine, ReadsTheMachineOfWellFormedHeaders) {
  for (const USHORT machine : std::initializer_list<USHORT>{
           IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_ARM64, IMAGE_FILE_MACHINE_I386, 0xA64E}) {
    USHORT read = 0;
    EXPECT_HRESULT_SUCCEEDED(Parse(MakePe({.machine = machine}), &read));
    EXPECT_EQ(read, machine);
  }
  // PE32 (32-bit optional header) as well as PE32+.
  USHORT read = 0;
  EXPECT_HRESULT_SUCCEEDED(Parse(MakePe({.machine = IMAGE_FILE_MACHINE_I386,
                                         .optionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC,
                                         .optionalSize = sizeof(IMAGE_OPTIONAL_HEADER32)}),
                                 &read));
  EXPECT_EQ(read, IMAGE_FILE_MACHINE_I386);
}

TEST(PeMachine, TruncatedFilesSayTruncated) {
  const std::vector<BYTE> whole = MakePe({});
  // Cut inside the DOS header, inside the PE header, and inside the optional header.
  for (const size_t size : {size_t{0}, size_t{1}, size_t{63}, size_t{0x80}, size_t{0x80 + 10},
                            size_t{0x80 + 24}, size_t{0x80 + 24 + 100}}) {
    const std::vector<BYTE> cut(whole.begin(), whole.begin() + size);
    USHORT machine = 1;
    EXPECT_EQ(Parse(cut, &machine), kTruncated) << size;
    EXPECT_EQ(machine, IMAGE_FILE_MACHINE_UNKNOWN) << size;
  }
  // The PE header offset points past the end of the file.
  USHORT machine = 0;
  EXPECT_EQ(Parse(MakePe({.peOffset = 0x1000, .fileSize = 0x1000}), &machine), kTruncated);
}

TEST(PeMachine, MalformedHeadersSayBadFormat) {
  USHORT machine = 0;
  EXPECT_EQ(Parse(MakePe({.dosMagic = 0x4D5A}), &machine), kBadFormat);  // "ZM"
  EXPECT_EQ(Parse(MakePe({.signature = 0x00004550 | 0x01000000}), &machine), kBadFormat);
  EXPECT_EQ(Parse(MakePe({.optionalMagic = 0x0107}), &machine), kBadFormat);  // ROM image
  EXPECT_EQ(Parse(MakePe({.optionalSize = 1}), &machine), kBadFormat);
  EXPECT_EQ(Parse(MakePe({.optionalSize = 0}), &machine), kBadFormat);
  // A negative offset.
  std::vector<BYTE> negative = MakePe({});
  Put(negative, offsetof(IMAGE_DOS_HEADER, e_lfanew), LONG{-4});
  EXPECT_EQ(Parse(negative, &machine), kBadFormat);
  // An offset of 0 makes "MZ" the signature.
  std::vector<BYTE> zero = MakePe({});
  Put(zero, offsetof(IMAGE_DOS_HEADER, e_lfanew), LONG{0});
  EXPECT_EQ(Parse(zero, &machine), kBadFormat);
  // Text, not an executable.
  const std::string text = "Not a DLL at all, just some text that is long enough for a header.";
  const std::vector<BYTE> bytes(text.begin(), text.end());
  EXPECT_EQ(Parse(bytes, &machine), kBadFormat);
  EXPECT_EQ(machine, IMAGE_FILE_MACHINE_UNKNOWN);
}

TEST(PeMachine, HeadersBeyondWhatIsReadAreRefused) {
  // The PE header lies within the file but beyond the bytes ReadPeMachine reads.
  const std::vector<BYTE> file = MakePe({.peOffset = 0x20000, .fileSize = 0x30000});
  USHORT machine = 0;
  EXPECT_EQ(ParsePeMachine(std::span<const BYTE>(file.data(), kPeHeadBytes), file.size(), &machine),
            kBadFormat);
  // The same file read whole parses.
  EXPECT_HRESULT_SUCCEEDED(ParsePeMachine(file, file.size(), &machine));
}

TEST(PeMachine, MachineToArch) {
  EXPECT_EQ(MachineToArch(IMAGE_FILE_MACHINE_AMD64), Arch::kX64);
  EXPECT_EQ(MachineToArch(IMAGE_FILE_MACHINE_ARM64), Arch::kArm64);
  EXPECT_EQ(MachineToArch(0xA64E), Arch::kArm64);  // ARM64X: holds native ARM64 code
  EXPECT_EQ(MachineToArch(0xA641), Arch::kOther);  // ARM64EC: not for native ARM64 processes
  EXPECT_EQ(MachineToArch(IMAGE_FILE_MACHINE_I386), Arch::kOther);
  EXPECT_EQ(MachineToArch(IMAGE_FILE_MACHINE_ARMNT), Arch::kOther);
  EXPECT_EQ(MachineToArch(IMAGE_FILE_MACHINE_UNKNOWN), Arch::kOther);
}

// ---- version resources ---------------------------------------------------------------------

void Pad4(std::vector<BYTE>& bytes) {
  while (bytes.size() % 4) bytes.push_back(0);
}

void AppendText(std::vector<BYTE>& bytes, std::wstring_view text) {
  for (const wchar_t c : text) {
    bytes.push_back(static_cast<BYTE>(c & 0xFF));
    bytes.push_back(static_cast<BYTE>(c >> 8));
  }
  bytes.push_back(0);
  bytes.push_back(0);
}

// One block: wLength, wValueLength, wType, the key, the value and the children, each aligned to
// 4 bytes as resource compilers write them.
std::vector<BYTE> Block(std::wstring_view key, WORD type, const std::vector<BYTE>& value,
                        WORD valueLength, const std::vector<std::vector<BYTE>>& children = {}) {
  std::vector<BYTE> bytes(6, 0);
  AppendText(bytes, key);
  Pad4(bytes);
  bytes.insert(bytes.end(), value.begin(), value.end());
  for (const std::vector<BYTE>& child : children) {
    Pad4(bytes);
    bytes.insert(bytes.end(), child.begin(), child.end());
  }
  Put(bytes, 0, static_cast<WORD>(bytes.size()));
  Put(bytes, 2, valueLength);
  Put(bytes, 4, type);
  return bytes;
}

std::vector<BYTE> StringBlock(std::wstring_view key, std::wstring_view text) {
  std::vector<BYTE> value;
  AppendText(value, text);
  return Block(key, 1, value, static_cast<WORD>(text.size() + 1));
}

std::vector<BYTE> Fixed(WORD a, WORD b, WORD c, WORD d) {
  VS_FIXEDFILEINFO info = {};
  info.dwSignature = 0xFEEF04BD;
  info.dwStrucVersion = 0x00010000;
  info.dwFileVersionMS = MAKELONG(b, a);
  info.dwFileVersionLS = MAKELONG(d, c);
  std::vector<BYTE> bytes(sizeof(info));
  std::memcpy(bytes.data(), &info, sizeof(info));
  return bytes;
}

std::vector<BYTE> Resource(const std::vector<BYTE>& fixed,
                           const std::vector<std::vector<BYTE>>& tables) {
  std::vector<BYTE> translation(4, 0);
  const std::vector<BYTE> var =
      Block(L"VarFileInfo", 1, {}, 0, {Block(L"Translation", 0, translation, 4)});
  return Block(L"VS_VERSION_INFO", 0, fixed, static_cast<WORD>(fixed.size()),
               {Block(L"StringFileInfo", 1, {}, 0, tables), var});
}

std::wstring ParseOk(const std::vector<BYTE>& resource) {
  std::wstring version;
  EXPECT_HRESULT_SUCCEEDED(ParseVersionResource(resource, &version));
  return version;
}

TEST(VersionResource, PrefersTheUsEnglishUnicodeTable) {
  const auto resource =
      Resource(Fixed(9, 9, 9, 9),
               {Block(L"040704b0", 1, {}, 0, {StringBlock(L"FileVersion", L"7.0.0.0")}),
                Block(L"040904b0", 1, {}, 0,
                      {StringBlock(L"CompanyName", L"x"), StringBlock(L"FileVersion", L"2.0.0.0"),
                       StringBlock(L"ProductVersion", L"2.0.0.0-dev")})});
  EXPECT_EQ(ParseOk(resource), L"2.0.0.0");
}

TEST(VersionResource, FallsBackToAnyTableThenTheFixedVersion) {
  EXPECT_EQ(
      ParseOk(Resource(Fixed(9, 9, 9, 9),
                       {Block(L"040704e4", 1, {}, 0, {StringBlock(L"FileVersion", L"1.1.0.0")})})),
      L"1.1.0.0");
  EXPECT_EQ(ParseOk(Resource(Fixed(1, 2, 3, 4), {})), L"1.2.3.4");
  // An empty FileVersion string does not count.
  EXPECT_EQ(ParseOk(Resource(Fixed(1, 2, 3, 4),
                             {Block(L"040904b0", 1, {}, 0, {StringBlock(L"FileVersion", L"")})})),
            L"1.2.3.4");
  // A system DLL's form, with a build tag after the number.
  EXPECT_EQ(ParseOk(Resource(
                Fixed(10, 0, 26100, 1),
                {Block(L"040904B0", 1, {}, 0,
                       {StringBlock(L"FileVersion", L"10.0.26100.1 (WinBuild.160101.0800)")})})),
            L"10.0.26100.1 (WinBuild.160101.0800)");
}

TEST(VersionResource, NothingToFind) {
  std::wstring version;
  // No fixed info (wrong signature) and no strings.
  std::vector<BYTE> fixed = Fixed(1, 0, 0, 0);
  Put(fixed, 0, DWORD{0});
  EXPECT_EQ(ParseVersionResource(Resource(fixed, {}), &version),
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
  EXPECT_EQ(ParseVersionResource(Resource({}, {}), &version), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
}

TEST(VersionResource, WriterThatCountsBytesInsteadOfCharacters) {
  // wValueLength in bytes makes the value seem twice as long; it must stay inside its block.
  std::vector<BYTE> value;
  AppendText(value, L"2.0.0.0");
  const auto string = Block(L"FileVersion", 1, value, static_cast<WORD>(value.size()));
  EXPECT_EQ(ParseOk(Resource(Fixed(9, 9, 9, 9), {Block(L"040904b0", 1, {}, 0, {string})})),
            L"2.0.0.0");
}

TEST(VersionResource, MalformedResourcesAreRefused) {
  const auto good = Resource(
      Fixed(2, 0, 0, 0), {Block(L"040904b0", 1, {}, 0, {StringBlock(L"FileVersion", L"2.0.0.0")})});
  std::wstring version;

  EXPECT_EQ(ParseVersionResource(std::span<const BYTE>(), &version), kBadResource);
  // Cut short: the root's length claims more than there is.
  const std::vector<BYTE> cut(good.begin(), good.begin() + good.size() / 2);
  EXPECT_EQ(ParseVersionResource(cut, &version), kBadResource);
  // Not a version resource.
  EXPECT_EQ(ParseVersionResource(Block(L"SOMETHING_ELSE", 0, {}, 0), &version), kBadResource);
  // A child longer than its parent.
  std::vector<BYTE> bigChild = good;
  const size_t childAt = 6 + (std::wstring_view(L"VS_VERSION_INFO").size() + 1) * 2;
  const size_t stringFileInfo = ((childAt + 3) & ~size_t{3}) + sizeof(VS_FIXEDFILEINFO);
  Put(bigChild, stringFileInfo, static_cast<WORD>(0x7FFF));
  EXPECT_EQ(ParseVersionResource(bigChild, &version), kBadResource);
  // A binary value longer than its block.
  std::vector<BYTE> bigValue = good;
  Put(bigValue, 2, static_cast<WORD>(0x7FFF));
  EXPECT_EQ(ParseVersionResource(bigValue, &version), kBadResource);
  // A key without its terminating null before the block ends.
  std::vector<BYTE> noNull(16, 0x41);
  Put(noNull, 0, static_cast<WORD>(16));
  EXPECT_EQ(ParseVersionResource(noNull, &version), kBadResource);
  EXPECT_TRUE(version.empty());
}

}  // namespace
}  // namespace pdft
