// The pure parts of the Smart App Control verdict (sac.h): the state value, the load-blocked
// errors, which events count, when to count and recount, and every row of the verdict table;
// and the build folder's path as Code Integrity names it, on folders in a private temporary
// folder. Reading the real state and the Code Integrity log is in the integration tests.

#include <windows.h>

#include "sac.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "temp_dir.h"

namespace pdft {
namespace {

RegistryValue Dword(DWORD number) {
  RegistryValue value;
  value.type = REG_DWORD;
  value.data.resize(sizeof(number));
  std::memcpy(value.data.data(), &number, sizeof(number));
  return value;
}

RegistryValue Text(const std::wstring& text) {
  RegistryValue value;
  value.type = REG_SZ;
  value.data.resize((text.size() + 1) * sizeof(wchar_t));
  std::memcpy(value.data.data(), text.c_str(), value.data.size());
  return value;
}

constexpr HRESULT Win32(DWORD code) { return HRESULT_FROM_WIN32(code); }

BlockCount Readable(int count) { return BlockCount{true, count, S_OK}; }
BlockCount Unreadable() { return BlockCount{false, 0, Win32(ERROR_EVT_CHANNEL_NOT_FOUND)}; }

TEST(SacState, ValueMeansState) {
  EXPECT_EQ(ClassifySacValue(std::nullopt).state, SacState::kAbsent);
  EXPECT_EQ(ClassifySacValue(Dword(0)).state, SacState::kOff);
  EXPECT_EQ(ClassifySacValue(Dword(1)).state, SacState::kOn);
  EXPECT_EQ(ClassifySacValue(Dword(2)).state, SacState::kEvaluation);

  const SacReading three = ClassifySacValue(Dword(3));
  EXPECT_EQ(three.state, SacState::kUnknown);
  EXPECT_EQ(three.value, 3u);
  EXPECT_EQ(SacValueText(three), L"3");

  // Only a 4-byte REG_DWORD is a state: "1" as text is not "on".
  const SacReading text = ClassifySacValue(Text(L"1"));
  EXPECT_EQ(text.state, SacState::kUnknown);
  EXPECT_FALSE(text.value);
  EXPECT_EQ(text.type, static_cast<DWORD>(REG_SZ));
  EXPECT_EQ(SacValueText(text), L"type 1");

  RegistryValue shortDword = Dword(1);
  shortDword.data.resize(2);
  EXPECT_EQ(ClassifySacValue(shortDword).state, SacState::kUnknown);

  EXPECT_EQ(SacValueText(ClassifySacValue(std::nullopt)), L"");
  EXPECT_EQ(SacValueText(ClassifySacValue(Dword(1))), L"1");
}

TEST(SacLoadBlocked, ExactlyThe1xSet) {
  const DWORD blocked[] = {577, 4551, 4556, 4557, 4558, 4559, 4580, 4581, 4582, 4583};
  for (const DWORD code : blocked) EXPECT_TRUE(IsLoadBlockedHresult(Win32(code))) << code;
  // The same codes as they appear in messages.
  EXPECT_TRUE(IsLoadBlockedHresult(static_cast<HRESULT>(0x80070241)));
  EXPECT_TRUE(IsLoadBlockedHresult(static_cast<HRESULT>(0x800711C7)));
  EXPECT_TRUE(IsLoadBlockedHresult(static_cast<HRESULT>(0x800711CC)));
  EXPECT_TRUE(IsLoadBlockedHresult(static_cast<HRESULT>(0x800711CF)));
  EXPECT_TRUE(IsLoadBlockedHresult(static_cast<HRESULT>(0x800711E4)));
  EXPECT_TRUE(IsLoadBlockedHresult(static_cast<HRESULT>(0x800711E7)));

  const DWORD neighbours[] = {576, 578, 4550, 4552, 4555, 4560, 4579, 4584};
  for (const DWORD code : neighbours) EXPECT_FALSE(IsLoadBlockedHresult(Win32(code))) << code;
  EXPECT_FALSE(IsLoadBlockedHresult(S_OK));
  EXPECT_FALSE(IsLoadBlockedHresult(E_FAIL));
  EXPECT_FALSE(IsLoadBlockedHresult(REGDB_E_CLASSNOTREG));
  EXPECT_FALSE(IsLoadBlockedHresult(Win32(ERROR_BAD_EXE_FORMAT)));
  // Only HRESULT_FROM_WIN32 codes: the same number in another facility, or as a plain Win32
  // error, is not one.
  EXPECT_FALSE(IsLoadBlockedHresult(static_cast<HRESULT>(0x80040241)));
  EXPECT_FALSE(IsLoadBlockedHresult(static_cast<HRESULT>(577)));
  EXPECT_FALSE(IsLoadBlockedHresult(static_cast<HRESULT>(0x00070241)));
}

// The build folder of a machine-wide install, as Code Integrity names it.
constexpr wchar_t kBuildFolder[] =
    L"\\Device\\HarddiskVolume3\\Program Files\\PdfThumbnails\\app\\2.0.0.0-1a2b3c4d";

TEST(SacBlockEvents, FileNamesThatCount) {
  const std::wstring folder = kBuildFolder;
  EXPECT_TRUE(FileNameIsInFolder(folder + L"\\PdfThumbnails.dll", folder));
  EXPECT_TRUE(FileNameIsInFolder(folder + L"\\licenses\\a.dll", folder));  // at any depth
  EXPECT_TRUE(FileNameIsInFolder(
      L"\\DEVICE\\HarddiskVolume3\\PROGRAM FILES\\PdfThumbnails\\APP\\2.0.0.0-1A2B3C4D\\"
      L"PdfThumbnails-pdfium.dll",
      folder));
  EXPECT_TRUE(FileNameIsInFolder(folder + L"\\a.dll", folder + L"\\"));

  // A folder of the same name somewhere else: another user's, on another volume, or the same
  // path further down.
  EXPECT_FALSE(FileNameIsInFolder(
      L"\\Device\\HarddiskVolume3\\Users\\someone\\app\\2.0.0.0-1a2b3c4d\\PdfThumbnails.dll",
      folder));
  EXPECT_FALSE(FileNameIsInFolder(
      L"\\Device\\HarddiskVolume4\\Program Files\\PdfThumbnails\\app\\2.0.0.0-1a2b3c4d\\a.dll",
      folder));
  EXPECT_FALSE(FileNameIsInFolder(
      L"\\Device\\HarddiskVolume3\\x\\Program Files\\PdfThumbnails\\app\\2.0.0.0-1a2b3c4d\\a.dll",
      folder));
  // Another build, a longer name that starts the same, the folder itself, nothing after the
  // separator.
  EXPECT_FALSE(FileNameIsInFolder(
      L"\\Device\\HarddiskVolume3\\Program Files\\PdfThumbnails\\app\\1.1.0.0-67a1323e\\a.dll",
      folder));
  EXPECT_FALSE(FileNameIsInFolder(folder + L"X\\a.dll", folder));
  EXPECT_FALSE(FileNameIsInFolder(folder, folder));
  EXPECT_FALSE(FileNameIsInFolder(folder + L"\\", folder));
  EXPECT_FALSE(FileNameIsInFolder(L"", folder));
  // No folder matches nothing, not everything.
  EXPECT_FALSE(FileNameIsInFolder(folder + L"\\a.dll", L""));
  EXPECT_FALSE(FileNameIsInFolder(folder + L"\\a.dll", L"\\"));
}

TEST(SacBlockEvents, EventsThatCount) {
  const std::wstring folder = kBuildFolder;
  // An event under the build folder counts.
  EXPECT_TRUE(EventCountsAsBlock(S_OK, folder + L"\\PdfThumbnails.dll", folder));
  // One under another "\app\<the same name>\" does not.
  EXPECT_FALSE(EventCountsAsBlock(
      S_OK, L"\\Device\\HarddiskVolume3\\Users\\someone\\app\\2.0.0.0-1a2b3c4d\\a.dll", folder));
  // One that could not be rendered is skipped, whatever the name it was given.
  EXPECT_FALSE(EventCountsAsBlock(Win32(ERROR_INSUFFICIENT_BUFFER), folder + L"\\PdfThumbnails.dll",
                                  folder));
  EXPECT_FALSE(EventCountsAsBlock(Win32(ERROR_INVALID_DATA), L"", folder));
}

TEST(SacBlockEvents, ALogNoEventOfWhichRendersIsUnreadable) {
  // An empty log, and one where a single odd event was passed over: read.
  EXPECT_EQ(RenderOutcome(0, 0, S_OK), S_OK);
  EXPECT_EQ(RenderOutcome(5, 1, Win32(ERROR_INSUFFICIENT_BUFFER)), S_OK);
  EXPECT_EQ(RenderOutcome(5, 0, S_OK), S_OK);
  // Events, none of which rendered: no answer, with the error when there is one.
  EXPECT_EQ(RenderOutcome(0, 3, Win32(ERROR_NOT_ENOUGH_MEMORY)), Win32(ERROR_NOT_ENOUGH_MEMORY));
  EXPECT_EQ(RenderOutcome(0, 3, S_OK), Win32(ERROR_INVALID_DATA));
}

TEST(SacBlockEvents, BuildFolderAsCodeIntegrityNamesIt) {
  test::TempDir dir(L"sac");
  ASSERT_TRUE(dir.ok());
  const std::wstring build = dir.File(L"app\\2.0.0.0-1a2b3c4d");
  ASSERT_TRUE(CreateDirectoryW(dir.File(L"app").c_str(), nullptr)) << GetLastError();
  ASSERT_TRUE(CreateDirectoryW(build.c_str(), nullptr)) << GetLastError();
  std::wstring device;
  ASSERT_HRESULT_SUCCEEDED(DeviceFolderPath(build, &device));
  EXPECT_TRUE(device.starts_with(L"\\Device\\")) << device;
  EXPECT_TRUE(device.ends_with(L"\\app\\2.0.0.0-1a2b3c4d")) << device;
  EXPECT_TRUE(FileNameIsInFolder(device + L"\\PdfThumbnails.dll", device));

  // A trailing separator, and a junction that leads to the folder, give the same path.
  std::wstring again;
  ASSERT_HRESULT_SUCCEEDED(DeviceFolderPath(build + L"\\", &again));
  EXPECT_EQ(again, device);
  ASSERT_TRUE(CreateDirectoryW(dir.File(L"link").c_str(), nullptr)) << GetLastError();
  ASSERT_TRUE(test::MakeJunction(dir.File(L"link"), build)) << GetLastError();
  ASSERT_HRESULT_SUCCEEDED(DeviceFolderPath(dir.File(L"link"), &again));
  EXPECT_EQ(again, device);

  // A missing folder, before the log is read.
  EXPECT_EQ(DeviceFolderPath(dir.File(L"missing"), &again),
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
  EXPECT_TRUE(again.empty());
  const BlockCount missing = CountCiBlocks(dir.File(L"missing"), kSacBlockDays);
  EXPECT_FALSE(missing.readable);
  EXPECT_EQ(missing.error, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
}

TEST(SacBlockEvents, OnlyDriveFoldersAreOpened) {
  for (const wchar_t* refused :
       {L"", L"2.0.0.0-1a2b3c4d", L"app\\2.0.0.0-1a2b3c4d", L"\\Windows", L"C:", L"C:\\",
        L"C:Windows", L"C:\\Windows:stream", L"\\\\server\\share\\app", L"\\\\?\\C:\\Windows",
        L"\\\\.\\pipe\\x", L"\\\\?\\GLOBALROOT\\Device\\x"}) {
    std::wstring device = L"left over";
    EXPECT_EQ(DeviceFolderPath(refused, &device), E_INVALIDARG) << refused;
    EXPECT_TRUE(device.empty()) << refused;
  }
}

TEST(SacBlockEvents, QueryOfTheDesign) {
  EXPECT_EQ(CiBlockQuery(7),
            L"*[System[(EventID=3077) and TimeCreated[timediff(@SystemTime) <= 604800000]]]");
  EXPECT_EQ(CiBlockQuery(1),
            L"*[System[(EventID=3077) and TimeCreated[timediff(@SystemTime) <= 86400000]]]");
}

TEST(SacBlockEvents, BadArgumentsAreUnreadable) {
  // A build folder's name alone (what 1.x matched) is not a folder.
  const BlockCount nameOnly = CountCiBlocks(L"2.0.0.0-1a2b3c4d", kSacBlockDays);
  EXPECT_FALSE(nameOnly.readable);
  EXPECT_EQ(nameOnly.error, E_INVALIDARG);
  const std::filesystem::path folder = L"C:\\Program Files\\PdfThumbnails\\app\\2.0.0.0-1a2b3c4d";
  for (const int days : {0, 366}) {
    const BlockCount count = CountCiBlocks(folder, days);
    EXPECT_FALSE(count.readable) << days;
    EXPECT_EQ(count.error, E_INVALIDARG) << days;
  }
}

TEST(SacCounting, WhenToCount) {
  // On, a class, and the test failed or did not run.
  EXPECT_TRUE(ShouldCountBlocks(SacState::kOn, true, std::nullopt));
  EXPECT_TRUE(ShouldCountBlocks(SacState::kOn, true, E_FAIL));
  // A passing test settles it; no class, or a class whose DLL is gone, means no build folder to
  // look for; other states never count.
  EXPECT_FALSE(ShouldCountBlocks(SacState::kOn, true, S_OK));
  EXPECT_FALSE(ShouldCountBlocks(SacState::kOn, false, E_FAIL));
  for (const SacState state :
       {SacState::kOff, SacState::kEvaluation, SacState::kAbsent, SacState::kUnknown}) {
    EXPECT_FALSE(ShouldCountBlocks(state, true, E_FAIL));
  }
}

TEST(SacCounting, WhenToCountAgain) {
  // 1.x: no blocks yet, the test ran and failed for another reason than a refused load.
  EXPECT_TRUE(ShouldRecountBlocks(E_FAIL, Readable(0)));
  EXPECT_TRUE(ShouldRecountBlocks(static_cast<HRESULT>(0x8004B200), Readable(0)));
  EXPECT_TRUE(ShouldRecountBlocks(E_FAIL, Unreadable()));  // 1.x read that as 0 too
  EXPECT_FALSE(ShouldRecountBlocks(E_FAIL, Readable(2)));
  EXPECT_FALSE(ShouldRecountBlocks(std::nullopt, Readable(0)));  // no test, nothing new to log
  EXPECT_FALSE(ShouldRecountBlocks(S_OK, Readable(0)));
  EXPECT_FALSE(ShouldRecountBlocks(Win32(4551), Readable(0)));  // the error already tells
  EXPECT_FALSE(ShouldRecountBlocks(Win32(577), Unreadable()));
}

TEST(SacVerdictTable, StatesOtherThanOn) {
  const std::optional<HRESULT> probes[] = {std::nullopt, S_OK, E_FAIL, Win32(4551)};
  const std::optional<BlockCount> counts[] = {std::nullopt, Readable(0), Readable(3), Unreadable()};
  for (const std::optional<HRESULT>& probe : probes) {
    for (const std::optional<BlockCount>& count : counts) {
      EXPECT_EQ(DecideSac(SacState::kEvaluation, probe, count).kind, SacVerdictKind::kEval);
      EXPECT_EQ(DecideSac(SacState::kOff, probe, count).kind, SacVerdictKind::kNone);
      EXPECT_EQ(DecideSac(SacState::kAbsent, probe, count).kind, SacVerdictKind::kNone);
      EXPECT_EQ(DecideSac(SacState::kUnknown, probe, count).kind, SacVerdictKind::kUnknown);
    }
  }
}

TEST(SacVerdictTable, OnAndTheTestPassed) {
  // Blocks are not counted then; even a count would not change it.
  EXPECT_EQ(DecideSac(SacState::kOn, S_OK, std::nullopt).kind, SacVerdictKind::kOk);
  EXPECT_EQ(DecideSac(SacState::kOn, S_OK, Readable(5)).kind, SacVerdictKind::kOk);
}

TEST(SacVerdictTable, OnAndBlockEvents) {
  for (const std::optional<HRESULT>& probe :
       {std::optional<HRESULT>{}, std::optional<HRESULT>{E_FAIL},
        std::optional<HRESULT>{Win32(4551)}}) {
    const SacVerdict verdict = DecideSac(SacState::kOn, probe, Readable(4));
    EXPECT_EQ(verdict.kind, SacVerdictKind::kBlocked);
    EXPECT_EQ(verdict.blocks, 4);
    EXPECT_FALSE(verdict.evidence);  // the events are the evidence
  }
}

TEST(SacVerdictTable, OnAndALoadBlockedError) {
  for (const std::optional<BlockCount>& count :
       {std::optional<BlockCount>{Readable(0)}, std::optional<BlockCount>{Unreadable()},
        std::optional<BlockCount>{}}) {
    for (const DWORD code : {577ul, 4551ul, 4583ul}) {
      const SacVerdict verdict = DecideSac(SacState::kOn, Win32(code), count);
      EXPECT_EQ(verdict.kind, SacVerdictKind::kBlocked) << code;
      EXPECT_EQ(verdict.blocks, 0);
      EXPECT_EQ(verdict.evidence, Win32(code));
    }
  }
}

TEST(SacVerdictTable, OnWithoutEvidence) {
  // Another error, or no test: what the log says.
  for (const std::optional<HRESULT>& probe :
       {std::optional<HRESULT>{}, std::optional<HRESULT>{E_FAIL},
        std::optional<HRESULT>{REGDB_E_CLASSNOTREG}}) {
    EXPECT_EQ(DecideSac(SacState::kOn, probe, Readable(0)).kind, SacVerdictKind::kOn);
    // Changed from 1.x: an unreadable log is not "no blocks".
    EXPECT_EQ(DecideSac(SacState::kOn, probe, Unreadable()).kind, SacVerdictKind::kOnUnknown);
    // Not counted (no class registered).
    EXPECT_EQ(DecideSac(SacState::kOn, probe, std::nullopt).kind, SacVerdictKind::kOn);
  }
}

}  // namespace
}  // namespace pdft
