#include "handler_host.h"

#include <shlwapi.h>

#include <cstdio>

#include "project_config.h"

namespace pdft::test {
namespace {

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using CanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

double NowMs() {
  LARGE_INTEGER frequency, now;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&now);
  return 1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// Copies a bitmap into top-down 32bpp pixels.
bool CopyPixels(HBITMAP bitmap, Rendered* out) {
  BITMAP bm = {};
  if (!GetObjectW(bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = bm.bmWidth;
  info.bmiHeader.biHeight = -bm.bmHeight;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  out->pixels.assign(static_cast<size_t>(bm.bmWidth) * bm.bmHeight, 0);
  HDC dc = GetDC(nullptr);
  const int lines =
      GetDIBits(dc, bitmap, 0, bm.bmHeight, out->pixels.data(), &info, DIB_RGB_COLORS);
  ReleaseDC(nullptr, dc);
  out->width = bm.bmWidth;
  out->height = bm.bmHeight;
  return lines == bm.bmHeight;
}

}  // namespace

bool Rendered::AllOpaque() const {
  for (uint32_t pixel : pixels) {
    if ((pixel >> 24) != 0xFF) return false;
  }
  return !pixels.empty();
}

HandlerHost::~HandlerHost() {
  if (factory_) factory_->Release();
}

HRESULT HandlerHost::Load() {
  if (factory_) return S_OK;
  wchar_t buffer[MAX_PATH];
  DWORD length = GetEnvironmentVariableW(kHandlerDllEnv, buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    path_ = buffer;
  } else {
    length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    path_.assign(buffer, length);
    path_ = path_.substr(0, path_.find_last_of(L'\\') + 1) + pdft::kHandlerDll;
  }
  module_ = LoadLibraryExW(path_.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module_) return HRESULT_FROM_WIN32(GetLastError());
  return GetClassObject(pdft::kHandlerClsid, IID_IClassFactory,
                        reinterpret_cast<void**>(&factory_));
}

HRESULT HandlerHost::GetClassObject(REFCLSID clsid, REFIID iid, void** object) const {
  auto function = reinterpret_cast<GetClassObjectFn>(
      reinterpret_cast<void*>(GetProcAddress(module_, "DllGetClassObject")));
  return function ? function(clsid, iid, object) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

HRESULT HandlerHost::CanUnloadNow() const {
  auto function = reinterpret_cast<CanUnloadNowFn>(
      reinterpret_cast<void*>(GetProcAddress(module_, "DllCanUnloadNow")));
  return function ? function() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

HRESULT HandlerHost::CreateProvider(IInitializeWithStream** init) const {
  if (!factory_) return E_UNEXPECTED;
  return factory_->CreateInstance(nullptr, pdft::kIidInitializeWithStream,
                                  reinterpret_cast<void**>(init));
}

Rendered HandlerHost::RenderStream(IStream* stream, UINT cx) const {
  Rendered result;
  const double start = NowMs();
  IInitializeWithStream* init = nullptr;
  IThumbnailProvider* provider = nullptr;
  HBITMAP bitmap = nullptr;
  result.hr = CreateProvider(&init);
  if (SUCCEEDED(result.hr)) result.hr = init->Initialize(stream, STGM_READ);
  if (SUCCEEDED(result.hr)) {
    result.hr = init->QueryInterface(pdft::kThumbSlotIid, reinterpret_cast<void**>(&provider));
  }
  if (SUCCEEDED(result.hr)) result.hr = provider->GetThumbnail(cx, &bitmap, &result.alpha);
  result.ms = NowMs() - start;
  if (SUCCEEDED(result.hr) && (!bitmap || !CopyPixels(bitmap, &result))) result.hr = E_UNEXPECTED;
  if (bitmap) DeleteObject(bitmap);
  if (provider) provider->Release();
  if (init) init->Release();
  return result;
}

Rendered HandlerHost::RenderFile(const std::wstring& file, UINT cx) const {
  IStream* stream = nullptr;
  const HRESULT hr = SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                            FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
  if (FAILED(hr)) {
    Rendered failed;
    failed.hr = hr;
    return failed;
  }
  Rendered result = RenderStream(stream, cx);
  stream->Release();
  return result;
}

bool UseZoom(const std::wstring& dir, int zoom) {
  const std::wstring path = dir + L"\\zoom-" + std::to_wstring(zoom) + L".ini";
  const std::string text = "[Thumbnails]\r\nZoom=" + std::to_string(zoom) + "\r\n";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  CloseHandle(file);
  return ok && SetEnvironmentVariableW(pdft::kLegacySettingsEnv, path.c_str());
}

std::string HresultText(HRESULT hr) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
  return text;
}

std::string Printable(const std::wstring& text) {
  std::string out;
  for (wchar_t c : text) out += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
  return out;
}

}  // namespace pdft::test
