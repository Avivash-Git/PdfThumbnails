// Where the zoom comes from (settings.h DecideZoom): the handler's precedence, decided without
// any registry or file access. The integration tests run the same cases through the registry.

#include <windows.h>

#include "settings.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>

namespace pdft {
namespace {

RegistryValue Dword(DWORD value) {
  RegistryValue raw;
  raw.type = REG_DWORD;
  raw.data.resize(sizeof(value));
  std::memcpy(raw.data.data(), &value, sizeof(value));
  return raw;
}

RegistryValue Text(const std::wstring& text) {
  RegistryValue raw;
  raw.type = REG_SZ;
  raw.data.resize((text.size() + 1) * sizeof(wchar_t));
  std::memcpy(raw.data.data(), text.c_str(), raw.data.size());
  return raw;
}

void ExpectDecision(const std::optional<RegistryValue>& registry, std::optional<int> ini, int zoom,
                    ZoomOrigin origin, ZoomValueState state) {
  const ZoomDecision decision = DecideZoom(registry, ini);
  EXPECT_EQ(decision.zoom, zoom);
  EXPECT_EQ(decision.origin, origin);
  EXPECT_EQ(decision.registry, state);
}

TEST(ZoomDecision, RegistryFirst) {
  ExpectDecision(Dword(130), std::nullopt, 130, ZoomOrigin::kRegistry, ZoomValueState::kValid);
  ExpectDecision(Dword(130), 120, 130, ZoomOrigin::kRegistry, ZoomValueState::kValid);
  ExpectDecision(Dword(100), 200, 100, ZoomOrigin::kRegistry, ZoomValueState::kValid);
  ExpectDecision(Dword(200), 120, 200, ZoomOrigin::kRegistry, ZoomValueState::kValid);
}

TEST(ZoomDecision, OutOfRangeIsClampedAndUsedLikeTheHandlerDoes) {
  ExpectDecision(Dword(250), 120, 200, ZoomOrigin::kRegistry, ZoomValueState::kOutOfRange);
  ExpectDecision(Dword(99), 120, 100, ZoomOrigin::kRegistry, ZoomValueState::kOutOfRange);
  ExpectDecision(Dword(0), std::nullopt, 100, ZoomOrigin::kRegistry, ZoomValueState::kOutOfRange);
  // A DWORD above INT_MAX is not negative: it clamps to the top.
  ExpectDecision(Dword(0xFFFFFFFB), std::nullopt, 200, ZoomOrigin::kRegistry,
                 ZoomValueState::kOutOfRange);
}

TEST(ZoomDecision, WrongTypeFallsThroughToTheIni) {
  ExpectDecision(Text(L"130"), 120, 120, ZoomOrigin::kLegacyIni, ZoomValueState::kWrongType);
  ExpectDecision(Text(L"130"), std::nullopt, 100, ZoomOrigin::kDefault, ZoomValueState::kWrongType);
  RegistryValue shortDword = Dword(130);
  shortDword.data.resize(2);
  ExpectDecision(shortDword, 150, 150, ZoomOrigin::kLegacyIni, ZoomValueState::kWrongType);
  RegistryValue qword;
  qword.type = REG_QWORD;
  qword.data.assign(8, 0);
  qword.data[0] = 130;
  ExpectDecision(qword, std::nullopt, 100, ZoomOrigin::kDefault, ZoomValueState::kWrongType);
  RegistryValue bigEndian = Dword(130);
  bigEndian.type = REG_DWORD_BIG_ENDIAN;
  ExpectDecision(bigEndian, 120, 120, ZoomOrigin::kLegacyIni, ZoomValueState::kWrongType);
}

TEST(ZoomDecision, IniThenDefault) {
  ExpectDecision(std::nullopt, 120, 120, ZoomOrigin::kLegacyIni, ZoomValueState::kMissing);
  ExpectDecision(std::nullopt, 100, 100, ZoomOrigin::kLegacyIni, ZoomValueState::kMissing);
  ExpectDecision(std::nullopt, std::nullopt, 100, ZoomOrigin::kDefault, ZoomValueState::kMissing);
  // ReadLegacyIniZoom clamps already; the decision keeps the result in range regardless.
  ExpectDecision(std::nullopt, 500, 200, ZoomOrigin::kLegacyIni, ZoomValueState::kMissing);
}

TEST(ZoomDecision, WhichStatesNeedRewriting) {
  EXPECT_FALSE(IsZoomValueInvalid(ZoomValueState::kMissing));
  EXPECT_FALSE(IsZoomValueInvalid(ZoomValueState::kValid));
  EXPECT_TRUE(IsZoomValueInvalid(ZoomValueState::kOutOfRange));
  EXPECT_TRUE(IsZoomValueInvalid(ZoomValueState::kWrongType));
}

}  // namespace
}  // namespace pdft
