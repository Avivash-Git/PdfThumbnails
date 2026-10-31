// The settings (settings.h) against the registry, through RegistryRoots that point at a private
// key per test, and 1.x settings files in a private temporary folder. Where the code under test
// would look for the user's real settings.ini, the tests point the handler's environment variable
// at a file of their own first, so the real one is never read.

#include <windows.h>

#include "settings.h"

#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <string>

#include "project_config.h"
#include "registry_sandbox.h"
#include "strings.h"
#include "temp_dir.h"
#include "zoom.h"

namespace pdft {
namespace {

// Writes an INI file as 1.x did (ASCII).
void WriteIni(const std::wstring& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

// Sets the handler's settings-file variable for the scope, then restores what was there.
class ScopedSettingsVariable {
 public:
  explicit ScopedSettingsVariable(const std::wstring& value) {
    wchar_t old[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(kLegacySettingsEnv, old, MAX_PATH);
    if (length > 0 && length < MAX_PATH) old_ = std::wstring(old, length);
    SetEnvironmentVariableW(kLegacySettingsEnv, value.c_str());
  }
  ScopedSettingsVariable(const ScopedSettingsVariable&) = delete;
  ScopedSettingsVariable& operator=(const ScopedSettingsVariable&) = delete;
  ~ScopedSettingsVariable() {
    SetEnvironmentVariableW(kLegacySettingsEnv, old_ ? old_->c_str() : nullptr);
  }

 private:
  std::optional<std::wstring> old_;
};

class SettingsStore : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    ASSERT_TRUE(temp_.ok());
    roots_ = {sandbox_.user(), sandbox_.machine()};
    missingIni_ = temp_.File(L"no-such-settings.ini");
  }

  void SetZoomValue(DWORD type, const void* data, DWORD size) {
    ASSERT_EQ(RegSetKeyValueW(sandbox_.user(), kRegKey, kZoomValueName, type, data, size),
              ERROR_SUCCESS);
  }

  ZoomDetails Load(const std::wstring& ini, Settings* settings = nullptr) {
    Settings local;
    ZoomDetails details;
    EXPECT_HRESULT_SUCCEEDED(
        LoadSettingsWithIni(roots_, ini, settings ? settings : &local, &details));
    return details;
  }

  test::ScopedRegistrySandbox sandbox_;
  test::TempDir temp_{L"settings"};
  RegistryRoots roots_;
  std::wstring missingIni_;
};

TEST_F(SettingsStore, SavedZoomComesFromTheRegistry) {
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 130));
  DWORD stored = 0;
  DWORD size = sizeof(stored);
  ASSERT_EQ(RegGetValueW(sandbox_.user(), kRegKey, kZoomValueName, RRF_RT_REG_DWORD, nullptr,
                         &stored, &size),
            ERROR_SUCCESS);
  EXPECT_EQ(stored, 130u);
  Settings settings;
  const ZoomDetails details = Load(missingIni_, &settings);
  EXPECT_EQ(settings.zoom, 130);
  EXPECT_EQ(details.decision.origin, ZoomOrigin::kRegistry);
  EXPECT_EQ(details.decision.registry, ZoomValueState::kValid);
  EXPECT_EQ(details.registryType, static_cast<DWORD>(REG_DWORD));
  EXPECT_EQ(details.registryRaw, 130u);
  // Saving clamps.
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 999));
  EXPECT_EQ(Load(missingIni_).decision.zoom, kMaxZoom);
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, -5));
  EXPECT_EQ(Load(missingIni_).decision.zoom, kMinZoom);
}

TEST_F(SettingsStore, RegistryBeatsTheIni) {
  const std::wstring ini = temp_.File(L"settings.ini");
  WriteIni(ini, "[Thumbnails]\r\nZoom=120\r\n");
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 130));
  const ZoomDetails details = Load(ini);
  EXPECT_EQ(details.decision.zoom, 130);
  EXPECT_EQ(details.decision.origin, ZoomOrigin::kRegistry);
  EXPECT_EQ(details.legacyIni.native(), ini);
  EXPECT_EQ(details.legacyIniZoom, 120);
}

TEST_F(SettingsStore, OutOfRangeIsReportedNotFixed) {
  const DWORD tooBig = 250;
  SetZoomValue(REG_DWORD, &tooBig, sizeof(tooBig));
  const ZoomDetails details = Load(missingIni_);
  EXPECT_EQ(details.decision.zoom, 200);
  EXPECT_EQ(details.decision.origin, ZoomOrigin::kRegistry);
  EXPECT_EQ(details.decision.registry, ZoomValueState::kOutOfRange);
  EXPECT_TRUE(IsZoomValueInvalid(details.decision.registry));
  EXPECT_EQ(details.registryRaw, 250u);
  // Loading did not rewrite it.
  DWORD stored = 0;
  DWORD size = sizeof(stored);
  ASSERT_EQ(RegGetValueW(sandbox_.user(), kRegKey, kZoomValueName, RRF_RT_REG_DWORD, nullptr,
                         &stored, &size),
            ERROR_SUCCESS);
  EXPECT_EQ(stored, 250u);
}

TEST_F(SettingsStore, WrongTypeFallsBackToTheIniAndIsReported) {
  const std::wstring ini = temp_.File(L"settings.ini");
  WriteIni(ini, "[Thumbnails]\r\nZoom=120\r\n");
  const wchar_t text[] = L"130";
  SetZoomValue(REG_SZ, text, sizeof(text));
  const ZoomDetails details = Load(ini);
  EXPECT_EQ(details.decision.zoom, 120);
  EXPECT_EQ(details.decision.origin, ZoomOrigin::kLegacyIni);
  EXPECT_EQ(details.decision.registry, ZoomValueState::kWrongType);
  EXPECT_EQ(details.registryType, static_cast<DWORD>(REG_SZ));
  EXPECT_FALSE(details.registryRaw);
}

TEST_F(SettingsStore, IniReadLikeTheHandler) {
  const std::wstring ini = temp_.File(L"settings.ini");
  struct Row {
    const char* text;
    std::optional<int> zoom;
  };
  const Row rows[] = {
      {"[Thumbnails]\r\nZoom=150\r\n", 150},
      {"[thumbnails]\r\nzoom=141\r\n", 141},  // names ignore case
      {"; comment\r\n[Thumbnails]\r\n; comment\r\nZoom=130\r\n", 130},
      {"[Thumbnails]\r\nZoom=250\r\n", 200},
      {"[Thumbnails]\r\nZoom=50\r\n", 100},
      {"[Thumbnails]\r\nZoom=-5\r\n", 200},  // the handler's unsigned read of "-5"
      {"[Thumbnails]\r\nZoom=abc\r\n", 100},
      {"[Thumbnails]\r\nZoom=\r\n", 100},
      {"[Thumbnails]\r\nOther=1\r\n", std::nullopt},
      {"[Other]\r\nZoom=150\r\n", std::nullopt},
      {"", std::nullopt},
  };
  for (const Row& row : rows) {
    WriteIni(ini, row.text);
    std::optional<int> zoom;
    ASSERT_HRESULT_SUCCEEDED(ReadLegacyIniZoom(ini, &zoom));
    EXPECT_EQ(zoom, row.zoom) << row.text;
    const ZoomDetails details = Load(ini);
    EXPECT_EQ(details.decision.origin, row.zoom ? ZoomOrigin::kLegacyIni : ZoomOrigin::kDefault)
        << row.text;
    EXPECT_EQ(details.decision.zoom, row.zoom.value_or(kMinZoom)) << row.text;
  }
  std::optional<int> zoom = 1;
  ASSERT_HRESULT_SUCCEEDED(ReadLegacyIniZoom(missingIni_, &zoom));
  EXPECT_FALSE(zoom);
  ASSERT_HRESULT_SUCCEEDED(ReadLegacyIniZoom(temp_.path(), &zoom));  // a folder
  EXPECT_FALSE(zoom);
  ASSERT_HRESULT_SUCCEEDED(ReadLegacyIniZoom(L"", &zoom));
  EXPECT_FALSE(zoom);
}

TEST_F(SettingsStore, TheHandlersEnvironmentVariableNamesTheIni) {
  const std::wstring ini = temp_.File(L"from-variable.ini");
  WriteIni(ini, "[Thumbnails]\r\nZoom=170\r\n");
  ScopedSettingsVariable variable(ini);
  std::filesystem::path path;
  ASSERT_HRESULT_SUCCEEDED(LegacyIniPath(&path));
  EXPECT_EQ(path.native(), ini);
  Settings settings;
  ZoomDetails details;
  ASSERT_HRESULT_SUCCEEDED(LoadSettings(roots_, &settings, &details));
  EXPECT_EQ(settings.zoom, 170);
  EXPECT_EQ(details.decision.origin, ZoomOrigin::kLegacyIni);
  EXPECT_EQ(details.legacyIni.native(), ini);
}

TEST_F(SettingsStore, DefaultWithoutRegistryOrIni) {
  ScopedSettingsVariable variable(missingIni_);
  Settings settings;
  ZoomDetails details;
  ASSERT_HRESULT_SUCCEEDED(LoadSettings(roots_, &settings, &details));
  EXPECT_EQ(settings.zoom, 100);
  EXPECT_TRUE(settings.confirmFullRefresh);
  EXPECT_EQ(details.decision.origin, ZoomOrigin::kDefault);
  EXPECT_EQ(details.decision.registry, ZoomValueState::kMissing);
  EXPECT_FALSE(details.legacyIniZoom);
}

TEST_F(SettingsStore, WithoutTheVariableTheIniIsTheHandlersDefaultPath) {
  ScopedSettingsVariable variable(L"");  // restores the variable afterwards
  SetEnvironmentVariableW(kLegacySettingsEnv, nullptr);
  std::filesystem::path path;
  ASSERT_HRESULT_SUCCEEDED(LegacyIniPath(&path));
  wchar_t localAppData[MAX_PATH] = {};
  ASSERT_NE(GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH), 0u);
  EXPECT_TRUE(IEquals(path.native(), std::wstring(localAppData) + kLegacySettingsIni))
      << WideToUtf8Lossy(path.native());
}

TEST_F(SettingsStore, ConfirmFullRefresh) {
  Settings settings;
  Load(missingIni_, &settings);
  EXPECT_TRUE(settings.confirmFullRefresh);
  ASSERT_HRESULT_SUCCEEDED(SaveConfirmFullRefresh(roots_, false));
  Load(missingIni_, &settings);
  EXPECT_FALSE(settings.confirmFullRefresh);
  ASSERT_HRESULT_SUCCEEDED(SaveConfirmFullRefresh(roots_, true));
  Load(missingIni_, &settings);
  EXPECT_TRUE(settings.confirmFullRefresh);
  // The wrong type keeps the safe default: ask first.
  const wchar_t text[] = L"0";
  ASSERT_EQ(RegSetKeyValueW(sandbox_.user(), kRegKey, kConfirmFullRefreshValueName, REG_SZ, text,
                            sizeof(text)),
            ERROR_SUCCESS);
  Load(missingIni_, &settings);
  EXPECT_TRUE(settings.confirmFullRefresh);
}

TEST_F(SettingsStore, CheckRecordRoundTrip) {
  std::optional<CheckRecord> read;
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &read));
  EXPECT_FALSE(read);

  CheckRecord record;
  GetSystemTimeAsFileTime(&record.time);
  record.result = ExitCode::kProblems;
  record.summary = L"slot .pdf filled; \x00e9t\x00e9";  // not only ASCII
  record.pdfcraftVersion = L"0.14.0";
  record.liveTest = static_cast<HRESULT>(0x800711C7);
  ASSERT_HRESULT_SUCCEEDED(RecordCheck(roots_, record));
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &read));
  ASSERT_TRUE(read);
  EXPECT_EQ(read->time.dwLowDateTime, record.time.dwLowDateTime);
  EXPECT_EQ(read->time.dwHighDateTime, record.time.dwHighDateTime);
  EXPECT_EQ(read->result, ExitCode::kProblems);
  EXPECT_EQ(read->summary, record.summary);
  EXPECT_EQ(read->pdfcraftVersion, L"0.14.0");
  EXPECT_EQ(read->liveTest, static_cast<HRESULT>(0x800711C7));

  // Stored as design says: QWORD time, DWORD result.
  const std::wstring state = StateKeyPath();
  EXPECT_EQ(state, std::wstring(kRegKey) + L"\\State");
  DWORD type = 0;
  ASSERT_EQ(RegGetValueW(sandbox_.user(), state.c_str(), L"LastCheckTime", RRF_RT_ANY, &type,
                         nullptr, nullptr),
            ERROR_SUCCESS);
  EXPECT_EQ(type, static_cast<DWORD>(REG_QWORD));

  // A later record without a live test drops the old result.
  record.result = ExitCode::kOk;
  record.liveTest.reset();
  record.pdfcraftVersion.clear();
  ASSERT_HRESULT_SUCCEEDED(RecordCheck(roots_, record));
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &read));
  ASSERT_TRUE(read);
  EXPECT_EQ(read->result, ExitCode::kOk);
  EXPECT_FALSE(read->liveTest);
  EXPECT_EQ(read->pdfcraftVersion, L"");
}

TEST_F(SettingsStore, CheckRecordsThatAreNotOursAreIgnored) {
  CheckRecord record;
  GetSystemTimeAsFileTime(&record.time);
  ASSERT_HRESULT_SUCCEEDED(RecordCheck(roots_, record));
  const std::wstring state = StateKeyPath();
  const DWORD unknown = 99;
  ASSERT_EQ(RegSetKeyValueW(sandbox_.user(), state.c_str(), L"LastCheckResult", REG_DWORD, &unknown,
                            sizeof(unknown)),
            ERROR_SUCCESS);
  std::optional<CheckRecord> read;
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &read));
  EXPECT_FALSE(read);
  // A time of the wrong type.
  ASSERT_HRESULT_SUCCEEDED(RecordCheck(roots_, record));
  const DWORD shortTime = 1;
  ASSERT_EQ(RegSetKeyValueW(sandbox_.user(), state.c_str(), L"LastCheckTime", REG_DWORD, &shortTime,
                            sizeof(shortTime)),
            ERROR_SUCCESS);
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &read));
  EXPECT_FALSE(read);
}

TEST_F(SettingsStore, MigratedFromV1) {
  bool migrated = true;
  ASSERT_HRESULT_SUCCEEDED(ReadMigratedFromV1(roots_, &migrated));
  EXPECT_FALSE(migrated);
  const std::wstring state = StateKeyPath();
  const DWORD one = 1;
  ASSERT_EQ(RegSetKeyValueW(sandbox_.user(), state.c_str(), L"MigratedFromV1", REG_DWORD, &one,
                            sizeof(one)),
            ERROR_SUCCESS);
  ASSERT_HRESULT_SUCCEEDED(ReadMigratedFromV1(roots_, &migrated));
  EXPECT_TRUE(migrated);
}

}  // namespace
}  // namespace pdft
