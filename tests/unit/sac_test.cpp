// The pure parts of the Smart App Control verdict (sac.h): the state value, the load-blocked
// errors, which event file names count, when to count and recount, and every row of the verdict
// table. Reading the real state and the Code Integrity log is in the integration tests.

#include <windows.h>

#include "sac.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

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

TEST(SacBlockEvents, FileNamesThatCount) {
  const std::wstring leaf = L"2.0.0.0-1a2b3c4d";
  EXPECT_TRUE(FileNameNamesBuild(
      L"\\Device\\HarddiskVolume3\\Program Files\\PdfThumbnails\\app\\2.0.0.0-1a2b3c4d\\"
      L"PdfThumbnails.dll",
      leaf));
  EXPECT_TRUE(FileNameNamesBuild(
      L"\\Device\\HarddiskVolume3\\Users\\a\\AppData\\Local\\Programs\\PdfThumbnails\\APP\\"
      L"2.0.0.0-1A2B3C4D\\PdfThumbnails-pdfium.dll",
      leaf));
  // Another build, a longer name that starts the same, no separator after the leaf, not below
  // an app folder.
  EXPECT_FALSE(
      FileNameNamesBuild(L"\\Device\\X\\PdfThumbnails\\app\\1.1.0.0-67a1323e\\a.dll", leaf));
  EXPECT_FALSE(
      FileNameNamesBuild(L"\\Device\\X\\PdfThumbnails\\app\\2.0.0.0-1a2b3c4dX\\a.dll", leaf));
  EXPECT_FALSE(FileNameNamesBuild(L"\\Device\\X\\PdfThumbnails\\app\\2.0.0.0-1a2b3c4d", leaf));
  EXPECT_FALSE(FileNameNamesBuild(L"\\Device\\X\\myapp\\2.0.0.0-1a2b3c4d\\a.dll", leaf));
  EXPECT_FALSE(FileNameNamesBuild(L"", leaf));
  EXPECT_FALSE(FileNameNamesBuild(L"\\app\\\\", L""));

  EXPECT_TRUE(IsBuildLeafName(leaf));
  EXPECT_TRUE(IsBuildLeafName(L"1.1.0.0-67a1323e"));
  for (const wchar_t* bad : {L"", L".", L"..", L"a\\b", L"a/b", L"C:", L"a*", L"a?", L"a\nb"}) {
    EXPECT_FALSE(IsBuildLeafName(bad)) << bad;
  }
  EXPECT_FALSE(IsBuildLeafName(std::wstring(MAX_PATH + 1, L'a')));
}

TEST(SacBlockEvents, QueryOfTheDesign) {
  EXPECT_EQ(CiBlockQuery(7),
            L"*[System[(EventID=3077) and TimeCreated[timediff(@SystemTime) <= 604800000]]]");
  EXPECT_EQ(CiBlockQuery(1),
            L"*[System[(EventID=3077) and TimeCreated[timediff(@SystemTime) <= 86400000]]]");
}

TEST(SacBlockEvents, BadArgumentsAreUnreadable) {
  const BlockCount noLeaf = CountCiBlocks(L"..", kSacBlockDays);
  EXPECT_FALSE(noLeaf.readable);
  EXPECT_EQ(noLeaf.error, E_INVALIDARG);
  EXPECT_FALSE(CountCiBlocks(L"2.0.0.0-1a2b3c4d", 0).readable);
  EXPECT_FALSE(CountCiBlocks(L"2.0.0.0-1a2b3c4d", 366).readable);
}

TEST(SacCounting, WhenToCount) {
  // On, a class, and the test failed or did not run.
  EXPECT_TRUE(ShouldCountBlocks(SacState::kOn, true, std::nullopt));
  EXPECT_TRUE(ShouldCountBlocks(SacState::kOn, true, E_FAIL));
  // A passing test settles it; no class means no build folder; other states never count.
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
