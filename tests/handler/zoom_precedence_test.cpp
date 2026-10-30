// Where the handler takes its zoom from, first match wins: the object's own preview override
// (IPdfThumbnailsPreviewOptions::SetZoom), the REG_DWORD HKCU\Software\PdfThumbnails\Zoom, the
// 1.x settings.ini, 100. Every source is clamped to 100..200, and a registry value of another
// type is ignored. HKCU is the run's private key (HandlerEnvironment); the tests write the value
// through that key's handle and delete it again after each test.
//
// 01-portrait is A4, 595 x 842 pt: at edge 256 it is 181 x 256 at zoom 100, 217 x 256 at 120,
// 235 x 256 at 130 and a 256 x 256 square from 142 on. Zoom 200 itself shows on 06-tall-strip
// (30 x 3000 pt) at edge 2560: 51 x 2560, where zoom 190 gives 49 x 2560 and an unclamped 1000
// would give 256 x 2560.

#include <windows.h>

#include <gtest/gtest.h>
#include <shlwapi.h>

#include <climits>
#include <string>
#include <string_view>

#include "handler_env.h"
#include "preview_override.h"
#include "project_config.h"
#include "zoom.h"

namespace pdft::test {
namespace {

struct Size {
  int w, h;
};

constexpr Size kA4At100 = {181, 256};
constexpr Size kA4At120 = {217, 256};
constexpr Size kA4At130 = {235, 256};
constexpr Size kSquare = {256, 256};
constexpr Size kStripAt200 = {51, 2560};  // 06- at edge 2560

::testing::AssertionResult HasSize(const Rendered& r, Size want) {
  if (r.hr != S_OK) return ::testing::AssertionFailure() << "render failed: " << HresultText(r.hr);
  if (r.width != want.w || r.height != want.h) {
    return ::testing::AssertionFailure()
           << r.width << "x" << r.height << ", expected " << want.w << "x" << want.h;
  }
  return ::testing::AssertionSuccess();
}

Rendered Render(std::wstring_view prefix, UINT cx = 256) {
  return HandlerEnvironment::Render(prefix, cx);
}

Rendered RenderAtZoom(std::wstring_view prefix, int zoom, UINT cx = 256) {
  return HandlerEnvironment::RenderAtZoom(prefix, zoom, cx);
}

// Points PDFTHUMBNAILS_SETTINGS at <run folder>\|name| holding |text| as it is, for settings files
// that UseZoom does not write. A ScopedZoom points it back at the run's own file afterwards.
bool UseSettingsText(const wchar_t* name, std::string_view text) {
  const std::wstring path = HandlerEnvironment::Folder() + L"\\" + name;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  CloseHandle(file);
  return ok && written == text.size() &&
         SetEnvironmentVariableW(pdft::kLegacySettingsEnv, path.c_str());
}

// One handler object over one file, kept alive across several GetThumbnail calls.
class Thumbnailer {
 public:
  explicit Thumbnailer(std::wstring_view prefix) {
    EXPECT_EQ(HandlerEnvironment::Host().CreateProvider(&init_), S_OK);
    IStream* stream = nullptr;
    EXPECT_EQ(SHCreateStreamOnFileEx(HandlerEnvironment::Pdf(prefix).c_str(),
                                     STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                     nullptr, &stream),
              S_OK);
    if (init_ && stream) {
      EXPECT_EQ(init_->Initialize(stream, STGM_READ), S_OK);
      EXPECT_EQ(init_->QueryInterface(pdft::kThumbSlotIid, reinterpret_cast<void**>(&thumbnails_)),
                S_OK);
    }
    if (stream) stream->Release();  // the object keeps its own reference
  }
  Thumbnailer(const Thumbnailer&) = delete;
  Thumbnailer& operator=(const Thumbnailer&) = delete;
  ~Thumbnailer() {
    if (thumbnails_) thumbnails_->Release();
    if (init_) init_->Release();
  }

  HRESULT SetZoom(int zoom) {
    if (!init_) return E_UNEXPECTED;
    pdft::IPdfThumbnailsPreviewOptions* options = nullptr;
    HRESULT hr = init_->QueryInterface(pdft::kPreviewIid, reinterpret_cast<void**>(&options));
    if (SUCCEEDED(hr)) {
      hr = options->SetZoom(zoom);
      options->Release();
    }
    return hr;
  }

  // GetThumbnail(cx): the result and the bitmap's size (no pixels).
  Rendered Render(UINT cx = 256) {
    Rendered result;
    if (!thumbnails_) return result;
    HBITMAP bitmap = nullptr;
    result.hr = thumbnails_->GetThumbnail(cx, &bitmap, &result.alpha);
    BITMAP info = {};
    if (bitmap && GetObjectW(bitmap, sizeof(info), &info)) {
      result.width = info.bmWidth;
      result.height = info.bmHeight;
    }
    if (bitmap) DeleteObject(bitmap);
    return result;
  }

 private:
  IInitializeWithStream* init_ = nullptr;
  IThumbnailProvider* thumbnails_ = nullptr;
};

// Each test starts without a registry value and ends by deleting it.
class ZoomPrecedence : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(HandlerEnvironment::Registry().ok());
    DeleteValue();
    DWORD size = 0;
    ASSERT_EQ(RegGetValueW(HKEY_CURRENT_USER, pdft::kRegKey, pdft::kZoomValueName, RRF_RT_ANY,
                           nullptr, nullptr, &size),
              ERROR_FILE_NOT_FOUND);
  }
  void TearDown() override { DeleteValue(); }

  // HKCU\<kRegKey>\Zoom, written through the private key's own handle.
  static LSTATUS WriteValue(DWORD type, const void* data, DWORD bytes) {
    return RegSetKeyValueW(HandlerEnvironment::Registry().key(), pdft::kRegKey,
                           pdft::kZoomValueName, type, data, bytes);
  }
  static LSTATUS WriteDword(DWORD zoom) { return WriteValue(REG_DWORD, &zoom, sizeof(zoom)); }
  static void DeleteValue() {
    RegDeleteKeyValueW(HandlerEnvironment::Registry().key(), pdft::kRegKey, pdft::kZoomValueName);
  }
};

TEST_F(ZoomPrecedence, WithoutARegistryValueTheSettingsFileDecides) {
  ScopedZoom ini(120);
  EXPECT_TRUE(HasSize(Render(L"01-"), kA4At120));
}

TEST_F(ZoomPrecedence, TheRegistryValueBeatsTheSettingsFile) {
  ScopedZoom ini(120);
  ASSERT_EQ(WriteDword(130), ERROR_SUCCESS);
  EXPECT_TRUE(HasSize(Render(L"01-"), kA4At130));
  // 100 is a value like any other, not "no value".
  ASSERT_EQ(WriteDword(100), ERROR_SUCCESS);
  EXPECT_TRUE(HasSize(Render(L"01-"), kA4At100));
}

TEST_F(ZoomPrecedence, ReadFreshForEveryThumbnail) {
  ScopedZoom ini(100);
  Thumbnailer object(L"01-");
  ASSERT_EQ(WriteDword(130), ERROR_SUCCESS);
  EXPECT_TRUE(HasSize(object.Render(), kA4At130));
  ASSERT_EQ(WriteDword(120), ERROR_SUCCESS);
  EXPECT_TRUE(HasSize(object.Render(), kA4At120));
  DeleteValue();
  EXPECT_TRUE(HasSize(object.Render(), kA4At100));
}

TEST_F(ZoomPrecedence, ThePreviewOverrideBeatsTheRegistry) {
  ScopedZoom ini(120);
  ASSERT_EQ(WriteDword(130), ERROR_SUCCESS);
  EXPECT_TRUE(HasSize(RenderAtZoom(L"01-", 200), kSquare));
  EXPECT_TRUE(HasSize(RenderAtZoom(L"01-", 100), kA4At100));
  EXPECT_TRUE(HasSize(RenderAtZoom(L"06-", 200, 2560), kStripAt200));
}

TEST_F(ZoomPrecedence, OtherValueTypesAreIgnored) {
  ScopedZoom ini(120);
  // Read as zoom 150, any of these would make A4 a square.
  const wchar_t text[] = L"150";
  const ULONGLONG qword = 150;
  const DWORD dword = 150;
  const WORD word = 150;
  const struct {
    const char* what;
    DWORD type;
    const void* data;
    DWORD bytes;
  } kValues[] = {
      {"REG_SZ \"150\"", REG_SZ, text, sizeof(text)},
      {"REG_EXPAND_SZ \"150\"", REG_EXPAND_SZ, text, sizeof(text)},
      {"REG_QWORD 150", REG_QWORD, &qword, sizeof(qword)},
      {"REG_BINARY of a DWORD 150", REG_BINARY, &dword, sizeof(dword)},
      {"REG_DWORD_BIG_ENDIAN", REG_DWORD_BIG_ENDIAN, &dword, sizeof(dword)},
      {"REG_DWORD of only 2 bytes", REG_DWORD, &word, sizeof(word)},
  };
  for (const auto& value : kValues) {
    ASSERT_EQ(WriteValue(value.type, value.data, value.bytes), ERROR_SUCCESS) << value.what;
    EXPECT_TRUE(HasSize(Render(L"01-"), kA4At120)) << value.what;
    DeleteValue();
  }
}

TEST_F(ZoomPrecedence, OutOfRangeRegistryValuesAreClamped) {
  ScopedZoom ini(120);
  for (DWORD zoom : {0ul, 1ul, 99ul}) {
    ASSERT_EQ(WriteDword(zoom), ERROR_SUCCESS);
    EXPECT_TRUE(HasSize(Render(L"01-"), kA4At100)) << zoom;
  }
  for (DWORD zoom : {201ul, 5000ul, 0x7FFFFFFFul, 0x80000000ul, 0xFFFFFFFFul}) {
    ASSERT_EQ(WriteDword(zoom), ERROR_SUCCESS);
    EXPECT_TRUE(HasSize(Render(L"01-"), kSquare)) << zoom;
    EXPECT_TRUE(HasSize(Render(L"06-", 2560), kStripAt200)) << zoom;
  }
}

TEST_F(ZoomPrecedence, TheSettingsFileIsReadAsIn1x) {
  ScopedZoom restore(100);  // afterwards the run's own settings file again
  // GetPrivateProfileIntW returns "-5" as the UINT 0xFFFFFFFB, so 1.x clamps it to 200, not 100.
  for (int zoom : {-5, 5000}) {
    ScopedZoom ini(zoom);
    EXPECT_TRUE(HasSize(Render(L"01-"), kSquare)) << zoom;
    EXPECT_TRUE(HasSize(Render(L"06-", 2560), kStripAt200)) << zoom;
  }
  // Text that is not a number reads as 0, and an empty value or another section gives the
  // default; either way the zoom is 100.
  const struct {
    const char* what;
    const char* text;
  } kFiles[] = {
      {"Zoom=abc", "[Thumbnails]\r\nZoom=abc\r\n"},
      {"Zoom=", "[Thumbnails]\r\nZoom=\r\n"},
      {"Zoom=150 in another section", "[Other]\r\nZoom=150\r\n"},
  };
  for (const auto& file : kFiles) {
    ASSERT_TRUE(UseSettingsText(L"raw.ini", file.text)) << file.what;
    EXPECT_TRUE(HasSize(Render(L"01-"), kA4At100)) << file.what;
  }
  // So does a settings file that does not exist.
  const std::wstring missing = HandlerEnvironment::Folder() + L"\\missing.ini";
  ASSERT_TRUE(SetEnvironmentVariableW(pdft::kLegacySettingsEnv, missing.c_str()));
  EXPECT_TRUE(HasSize(Render(L"01-"), kA4At100));
}

TEST_F(ZoomPrecedence, SetZoomIsClamped) {
  ScopedZoom ini(120);
  ASSERT_EQ(WriteDword(130), ERROR_SUCCESS);
  // An unclamped -5 would make A4 a square; 100 shows the whole page.
  EXPECT_TRUE(HasSize(RenderAtZoom(L"01-", -5), kA4At100));
  EXPECT_TRUE(HasSize(RenderAtZoom(L"01-", 0), kA4At100));
  EXPECT_TRUE(HasSize(RenderAtZoom(L"01-", INT_MIN), kA4At100));
  EXPECT_TRUE(HasSize(RenderAtZoom(L"06-", 1000, 2560), kStripAt200));
  EXPECT_TRUE(HasSize(RenderAtZoom(L"06-", INT_MAX, 2560), kStripAt200));
}

TEST_F(ZoomPrecedence, TheOverrideBelongsToOneObject) {
  ScopedZoom ini(120);
  ASSERT_EQ(WriteDword(130), ERROR_SUCCESS);
  Thumbnailer preview(L"01-"), other(L"01-");
  EXPECT_TRUE(HasSize(preview.Render(), kA4At130));
  // From SetZoom on, this object ignores the saved zoom; the other one keeps following it.
  ASSERT_EQ(preview.SetZoom(200), S_OK);
  EXPECT_TRUE(HasSize(preview.Render(), kSquare));
  EXPECT_TRUE(HasSize(other.Render(), kA4At130));
  ASSERT_EQ(WriteDword(120), ERROR_SUCCESS);
  EXPECT_TRUE(HasSize(preview.Render(), kSquare));
  EXPECT_TRUE(HasSize(other.Render(), kA4At120));
  // A later SetZoom replaces the earlier one.
  ASSERT_EQ(preview.SetZoom(100), S_OK);
  EXPECT_TRUE(HasSize(preview.Render(), kA4At100));
  // New objects never inherit an override.
  EXPECT_TRUE(HasSize(Render(L"01-"), kA4At120));
}

// ---- The private interface ------------------------------------------------------------------

TEST(PreviewOptions, QueryInterface) {
  const HandlerHost& host = HandlerEnvironment::Host();
  pdft::IPdfThumbnailsPreviewOptions* options = nullptr;
  // Straight from the class factory, as the settings app's preview may ask.
  ASSERT_EQ(host.factory()->CreateInstance(nullptr, pdft::kPreviewIid,
                                           reinterpret_cast<void**>(&options)),
            S_OK);
  ASSERT_NE(options, nullptr);
  EXPECT_EQ(options->SetZoom(150), S_OK);

  // COM identity: IUnknown is the same pointer whichever interface it is asked from.
  IUnknown* unknownFromOptions = nullptr;
  ASSERT_EQ(options->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unknownFromOptions)),
            S_OK);
  IInitializeWithStream* init = nullptr;
  ASSERT_EQ(
      options->QueryInterface(pdft::kIidInitializeWithStream, reinterpret_cast<void**>(&init)),
      S_OK);
  IUnknown* unknownFromInit = nullptr;
  ASSERT_EQ(init->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unknownFromInit)), S_OK);
  EXPECT_EQ(unknownFromOptions, unknownFromInit);
  IThumbnailProvider* thumbnails = nullptr;
  ASSERT_EQ(options->QueryInterface(pdft::kThumbSlotIid, reinterpret_cast<void**>(&thumbnails)),
            S_OK);
  pdft::IPdfThumbnailsPreviewOptions* again = nullptr;
  ASSERT_EQ(thumbnails->QueryInterface(pdft::kPreviewIid, reinterpret_cast<void**>(&again)), S_OK);
  EXPECT_EQ(again, options);

  // Only the exact IID: one bit off is just another unknown interface.
  IID nearMiss = pdft::kPreviewIid;
  nearMiss.Data4[7] = static_cast<unsigned char>(nearMiss.Data4[7] ^ 1);
  void* object = &nearMiss;
  EXPECT_EQ(options->QueryInterface(nearMiss, &object), E_NOINTERFACE);
  EXPECT_EQ(object, nullptr);
  object = &nearMiss;
  EXPECT_EQ(options->QueryInterface(IID_IPersistStream, &object), E_NOINTERFACE);
  EXPECT_EQ(object, nullptr);
  EXPECT_EQ(options->QueryInterface(pdft::kPreviewIid, nullptr), E_POINTER);

  again->Release();
  thumbnails->Release();
  unknownFromInit->Release();
  init->Release();
  unknownFromOptions->Release();
  EXPECT_EQ(host.CanUnloadNow(), S_FALSE) << "one reference is left";
  options->Release();
  EXPECT_EQ(host.CanUnloadNow(), S_OK) << "every reference is released";
}

}  // namespace
}  // namespace pdft::test
