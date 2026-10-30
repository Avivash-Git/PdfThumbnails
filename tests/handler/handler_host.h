// Loads PdfThumbnails.dll into the test process and renders through its COM objects, exactly as
// Explorer's thumbnail process does (IInitializeWithStream + IThumbnailProvider), without any
// registration.

#pragma once

#include <windows.h>

#include <propsys.h>
#include <shlobj.h>
#include <thumbcache.h>

#include <cstdint>
#include <string>
#include <vector>

#ifndef WTS_E_FAILEDEXTRACTION
#define WTS_E_FAILEDEXTRACTION ((HRESULT)0x8004B200L)
#endif
#ifndef WTS_E_EXTRACTIONTIMEDOUT
#define WTS_E_EXTRACTIONTIMEDOUT ((HRESULT)0x8004B201L)
#endif

namespace pdft::test {

// The handler DLL to test: PDFT_TEST_HANDLER_DLL if set (e.g. an older build, to compare),
// else PdfThumbnails.dll next to the test program.
inline constexpr wchar_t kHandlerDllEnv[] = L"PDFT_TEST_HANDLER_DLL";

// One GetThumbnail result, with the bitmap copied out as top-down 0xAARRGGBB pixels.
struct Rendered {
  HRESULT hr = E_FAIL;
  int width = 0;
  int height = 0;
  WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
  std::vector<uint32_t> pixels;
  double ms = 0;

  uint32_t At(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
  bool AllOpaque() const;
};

class HandlerHost {
 public:
  HandlerHost() = default;
  HandlerHost(const HandlerHost&) = delete;
  HandlerHost& operator=(const HandlerHost&) = delete;
  ~HandlerHost();

  // Loads the DLL and gets its class factory. The DLL stays loaded (it pins itself anyway).
  HRESULT Load();
  const std::wstring& path() const { return path_; }

  // DllGetClassObject / DllCanUnloadNow as exported.
  HRESULT GetClassObject(REFCLSID clsid, REFIID iid, void** object) const;
  HRESULT CanUnloadNow() const;
  IClassFactory* factory() const { return factory_; }

  // A new handler object, asked for IInitializeWithStream.
  HRESULT CreateProvider(IInitializeWithStream** init) const;

  // Initialize(stream) + GetThumbnail(cx) on a new object.
  Rendered RenderStream(IStream* stream, UINT cx) const;
  // The same on a read-only stream over |file|.
  Rendered RenderFile(const std::wstring& file, UINT cx) const;

 private:
  std::wstring path_;
  HMODULE module_ = nullptr;
  IClassFactory* factory_ = nullptr;
};

// Points PDFTHUMBNAILS_SETTINGS at <dir>\zoom-<zoom>.ini ([Thumbnails] Zoom=<zoom>), so the
// handler never reads the user's own settings. False when the file cannot be written.
bool UseZoom(const std::wstring& dir, int zoom);

// "0x8004B200" etc.
std::string HresultText(HRESULT hr);
// ASCII-only rendering of a file name for messages.
std::string Printable(const std::wstring& text);

}  // namespace pdft::test
