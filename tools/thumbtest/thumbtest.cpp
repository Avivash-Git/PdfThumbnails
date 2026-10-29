// thumbtest: checks PdfThumbnails.dll against real files.
//
//   thumbtest direct <PdfThumbnails.dll> <size> <outdir|-> <file.pdf>...
//       Calls the DLL directly; no registration needed.
//   thumbtest cache <size> <outdir|-> <file.pdf>...
//       Forces a fresh extraction through Explorer's thumbnail cache, i.e. the
//       registered handler running in the isolated thumbnail process.
//   thumbtest cachenew <size> <outdir|-> <file.pdf>...
//       Same, but in a new thumbnail process (picks up a rebuilt DLL).
//   thumbtest cacheinproc <size> <outdir|-> <file.pdf>...
//       Same, but the cache loads the handler into this process instead.
//   thumbtest shell <size> <outdir|-> <file.pdf>...
//       Asks the shell for the thumbnail Explorer would show (may be cached).
//   thumbtest stress <PdfThumbnails.dll> <size> <threads> <rounds> <file.pdf>...
//       Renders the files concurrently from several threads.
//
// Prints one line per file: OK/FAIL, bitmap size, share of non-white pixels,
// alpha type and milliseconds. With an outdir it also writes <name>.png there.

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr CLSID kClsidPdfThumbnails = {
    0x0c3534c4, 0x5b7a, 0x48ab, {0x82, 0x8d, 0x1c, 0xd6, 0x60, 0xdc, 0x44, 0x7f}};
constexpr IID kIidThumbnailProvider = {
    0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};
constexpr IID kIidInitializeWithStream = {
    0xb824b49d, 0x22ac, 0x4161, {0xac, 0x8a, 0x99, 0x16, 0xe8, 0xfa, 0x3f, 0x7f}};
constexpr CLSID kClsidLocalThumbnailCache = {
    0x50ef4544, 0xac9f, 0x4a8e, {0xb2, 0x1b, 0x8a, 0x26, 0x18, 0x0d, 0xb1, 0x3f}};
constexpr IID kIidThumbnailCache = {
    0xf676c15d, 0x596a, 0x4ce2, {0x82, 0x34, 0x33, 0x99, 0x6f, 0x44, 0x5d, 0xb1}};
constexpr IID kIidShellItem = {
    0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
constexpr IID kIidShellItemImageFactory = {
    0xbcc18b79, 0xba16, 0x442f, {0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b}};
constexpr CLSID kClsidWicImagingFactory = {
    0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
constexpr IID kIidWicImagingFactory = {
    0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
constexpr GUID kContainerPng = {
    0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
constexpr GUID kPixel32bppBgr = {
    0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e}};
constexpr GUID kPixel24bppBgr = {
    0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};

// Minimal owning COM pointer.
template <typename T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() {
    if (p) p->Release();
  }
  void** out() { return reinterpret_cast<void**>(&p); }
  T* operator->() const { return p; }
};

std::mutex g_printLock;

std::string Utf8(const std::wstring& text) {
  int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &out[0], size, nullptr, nullptr);
  return out;
}

std::wstring Stem(const std::wstring& path) {
  std::wstring name = PathFindFileNameW(path.c_str());
  size_t dot = name.rfind(L'.');
  return dot == std::wstring::npos ? name : name.substr(0, dot);
}

double NowMs() {
  static LARGE_INTEGER frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return 1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// Copies |bitmap| into a top-down 32bpp BGRA buffer.
bool ReadPixels(HBITMAP bitmap, std::vector<DWORD>* pixels, int* width, int* height) {
  BITMAP bm = {};
  if (!GetObjectW(bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = bm.bmWidth;
  info.bmiHeader.biHeight = -bm.bmHeight;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  pixels->assign(static_cast<size_t>(bm.bmWidth) * bm.bmHeight, 0);
  HDC dc = GetDC(nullptr);
  int lines = GetDIBits(dc, bitmap, 0, bm.bmHeight, pixels->data(), &info, DIB_RGB_COLORS);
  ReleaseDC(nullptr, dc);
  *width = bm.bmWidth;
  *height = bm.bmHeight;
  return lines == bm.bmHeight;
}

// Percentage of pixels that are not (nearly) white.
double InkShare(const std::vector<DWORD>& pixels) {
  size_t ink = 0;
  for (DWORD p : pixels) {
    if ((p & 0xFF) < 0xF0 || ((p >> 8) & 0xFF) < 0xF0 || ((p >> 16) & 0xFF) < 0xF0) ++ink;
  }
  return pixels.empty() ? 0.0 : 100.0 * static_cast<double>(ink) / static_cast<double>(pixels.size());
}

bool AllOpaque(const std::vector<DWORD>& pixels) {
  for (DWORD p : pixels) {
    if ((p >> 24) != 0xFF) return false;
  }
  return true;
}

HRESULT SavePng(const std::vector<DWORD>& pixels, int width, int height, const std::wstring& path) {
  Com<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                kIidWicImagingFactory, factory.out());
  Com<IWICBitmap> bitmap;
  if (SUCCEEDED(hr)) {
    hr = factory->CreateBitmapFromMemory(width, height, kPixel32bppBgr, width * 4,
                                         static_cast<UINT>(pixels.size() * 4),
                                         reinterpret_cast<BYTE*>(const_cast<DWORD*>(pixels.data())),
                                         &bitmap.p);
  }
  Com<IWICFormatConverter> converter;
  if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter.p);
  if (SUCCEEDED(hr)) {
    hr = converter->Initialize(bitmap.p, kPixel24bppBgr, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom);
  }
  Com<IWICStream> stream;
  if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream.p);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  Com<IWICBitmapEncoder> encoder;
  if (SUCCEEDED(hr)) hr = factory->CreateEncoder(kContainerPng, nullptr, &encoder.p);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.p, WICBitmapEncoderNoCache);
  Com<IWICBitmapFrameEncode> frame;
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame.p, nullptr);
  if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
  if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
  WICPixelFormatGUID format = kPixel24bppBgr;
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr)) hr = frame->WriteSource(converter.p, nullptr);
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  return hr;
}

const char* AlphaName(int alpha) {
  switch (alpha) {
    case WTSAT_RGB: return "RGB ";
    case WTSAT_ARGB: return "ARGB";
    case WTSAT_UNKNOWN: return "UNK ";
    default: return "-   ";
  }
}

void Report(const std::wstring& file, HRESULT hr, HBITMAP bitmap, int alpha, double ms,
            const std::wstring& outdir) {
  std::lock_guard<std::mutex> lock(g_printLock);
  std::string name = Utf8(PathFindFileNameW(file.c_str()));
  std::vector<DWORD> pixels;
  int width = 0, height = 0;
  if (FAILED(hr) || !bitmap || !ReadPixels(bitmap, &pixels, &width, &height)) {
    std::printf("FAIL 0x%08lX                            %7.0f ms  %s\n",
                static_cast<unsigned long>(hr), ms, name.c_str());
    return;
  }
  std::printf("OK   %4dx%-4d  ink %5.1f%%  %s %s  %7.0f ms  %s\n", width, height,
              InkShare(pixels), AlphaName(alpha), AllOpaque(pixels) ? "opaque" : "      ", ms,
              name.c_str());
  if (outdir != L"-") {
    HRESULT saved = SavePng(pixels, width, height, outdir + L"\\" + Stem(file) + L".png");
    if (FAILED(saved)) std::printf("     (could not write png: 0x%08lX)\n", static_cast<unsigned long>(saved));
  }
  std::fflush(stdout);
}

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

IClassFactory* LoadFactory(const wchar_t* dllPath) {
  HMODULE dll = LoadLibraryExW(dllPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) {
    std::printf("cannot load %s (error %lu)\n", Utf8(dllPath).c_str(), GetLastError());
    return nullptr;
  }
  auto getClassObject = reinterpret_cast<GetClassObjectFn>(
      reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
  IClassFactory* factory = nullptr;
  if (!getClassObject || FAILED(getClassObject(kClsidPdfThumbnails, IID_IClassFactory,
                                               reinterpret_cast<void**>(&factory)))) {
    std::printf("DllGetClassObject failed\n");
    return nullptr;
  }
  return factory;
}

HRESULT ExtractDirect(IClassFactory* factory, const std::wstring& file, UINT size,
                      HBITMAP* bitmap, WTS_ALPHATYPE* alpha) {
  Com<IStream> stream;
  HRESULT hr = SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                      FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream.p);
  Com<IInitializeWithStream> init;
  if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, kIidInitializeWithStream, init.out());
  if (SUCCEEDED(hr)) hr = init->Initialize(stream.p, STGM_READ);
  Com<IThumbnailProvider> provider;
  if (SUCCEEDED(hr)) hr = init->QueryInterface(kIidThumbnailProvider, provider.out());
  if (SUCCEEDED(hr)) hr = provider->GetThumbnail(size, bitmap, alpha);
  return hr;
}

int RunDirect(const wchar_t* dllPath, UINT size, const std::wstring& outdir,
              const std::vector<std::wstring>& files) {
  IClassFactory* factory = LoadFactory(dllPath);
  if (!factory) return 2;
  int failures = 0;
  for (const auto& file : files) {
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    double start = NowMs();
    HRESULT hr = ExtractDirect(factory, file, size, &bitmap, &alpha);
    Report(file, hr, bitmap, alpha, NowMs() - start, outdir);
    if (bitmap) DeleteObject(bitmap);
    if (FAILED(hr)) ++failures;
  }
  factory->Release();
  return failures ? 1 : 0;
}

int RunCache(UINT size, int extraFlags, const std::wstring& outdir,
             const std::vector<std::wstring>& files) {
  const int flags = WTS_EXTRACT | WTS_FORCEEXTRACTION | extraFlags;
  Com<IThumbnailCache> cache;
  HRESULT hr = CoCreateInstance(kClsidLocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER,
                                kIidThumbnailCache, cache.out());
  if (FAILED(hr)) {
    std::printf("cannot create the thumbnail cache: 0x%08lX\n", static_cast<unsigned long>(hr));
    return 2;
  }
  int failures = 0;
  for (const auto& file : files) {
    double start = NowMs();
    Com<IShellItem> item;
    Com<ISharedBitmap> shared;
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    hr = SHCreateItemFromParsingName(file.c_str(), nullptr, kIidShellItem, item.out());
    if (SUCCEEDED(hr)) {
      hr = cache->GetThumbnail(item.p, size, static_cast<WTS_FLAGS>(flags), &shared.p, nullptr, nullptr);
    }
    if (SUCCEEDED(hr) && shared.p) hr = shared->GetSharedBitmap(&bitmap);  // owned by |shared|
    if (SUCCEEDED(hr) && shared.p) shared->GetFormat(&alpha);
    if (SUCCEEDED(hr) && !shared.p) hr = E_FAIL;
    Report(file, hr, bitmap, alpha, NowMs() - start, outdir);
    if (FAILED(hr)) ++failures;
  }
  return failures ? 1 : 0;
}

int RunShell(UINT size, const std::wstring& outdir, const std::vector<std::wstring>& files) {
  int failures = 0;
  for (const auto& file : files) {
    double start = NowMs();
    Com<IShellItemImageFactory> images;
    HBITMAP bitmap = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(file.c_str(), nullptr, kIidShellItemImageFactory,
                                             images.out());
    SIZE wanted = {static_cast<LONG>(size), static_cast<LONG>(size)};
    if (SUCCEEDED(hr)) hr = images->GetImage(wanted, SIIGBF_THUMBNAILONLY, &bitmap);
    Report(file, hr, bitmap, -1, NowMs() - start, outdir);
    if (bitmap) DeleteObject(bitmap);
    if (FAILED(hr)) ++failures;
  }
  return failures ? 1 : 0;
}

int RunStress(const wchar_t* dllPath, UINT size, int threads, int rounds,
              const std::vector<std::wstring>& files) {
  IClassFactory* factory = LoadFactory(dllPath);
  if (!factory) return 2;
  struct Tally {
    int ok = 0, fail = 0;
    int width = -1, height = -1;
    bool inconsistent = false;
  };
  std::map<std::wstring, Tally> tallies;
  std::mutex tallyLock;
  std::atomic<int> next{0};
  const int total = rounds * static_cast<int>(files.size());
  double start = NowMs();
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      for (int i = next++; i < total; i = next++) {
        const std::wstring& file = files[i % files.size()];
        HBITMAP bitmap = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        HRESULT hr = ExtractDirect(factory, file, size, &bitmap, &alpha);
        BITMAP bm = {};
        if (bitmap) GetObjectW(bitmap, sizeof(bm), &bm);
        {
          std::lock_guard<std::mutex> lock(tallyLock);
          Tally& tally = tallies[file];
          if (SUCCEEDED(hr) && bitmap) {
            ++tally.ok;
            if (tally.width >= 0 && (tally.width != bm.bmWidth || tally.height != bm.bmHeight))
              tally.inconsistent = true;
            tally.width = bm.bmWidth;
            tally.height = bm.bmHeight;
          } else {
            ++tally.fail;
          }
        }
        if (bitmap) DeleteObject(bitmap);
      }
      CoUninitialize();
    });
  }
  for (auto& worker : workers) worker.join();
  std::printf("%d renders on %d threads in %.0f ms\n", total, threads, NowMs() - start);
  bool bad = false;
  for (const auto& entry : tallies) {
    const Tally& tally = entry.second;
    std::printf("  ok %4d  fail %4d  %s%s\n", tally.ok, tally.fail,
                Utf8(PathFindFileNameW(entry.first.c_str())).c_str(),
                tally.inconsistent ? "  INCONSISTENT SIZES" : "");
    bad = bad || tally.inconsistent || (tally.ok && tally.fail);
  }
  factory->Release();
  return bad ? 1 : 0;
}

int Usage() {
  std::printf(
      "usage:\n"
      "  thumbtest direct <PdfThumbnails.dll> <size> <outdir|-> <file.pdf>...\n"
      "  thumbtest cache|cachenew|cacheinproc <size> <outdir|-> <file.pdf>...\n"
      "  thumbtest shell <size> <outdir|-> <file.pdf>...\n"
      "  thumbtest stress <PdfThumbnails.dll> <size> <threads> <rounds> <file.pdf>...\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return Usage();
  const std::wstring mode = argv[1];
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  auto filesFrom = [&](int first) {
    std::vector<std::wstring> files;
    for (int i = first; i < argc; ++i) files.emplace_back(argv[i]);
    return files;
  };
  int result = 2;
  if (mode == L"direct" && argc >= 6) {
    result = RunDirect(argv[2], static_cast<UINT>(_wtoi(argv[3])), argv[4], filesFrom(5));
  } else if ((mode == L"cache" || mode == L"cachenew" || mode == L"cacheinproc") && argc >= 5) {
    const int extraFlags = mode == L"cachenew"      ? WTS_INSTANCESURROGATE
                           : mode == L"cacheinproc" ? WTS_EXTRACTINPROC
                                                    : 0;
    result = RunCache(static_cast<UINT>(_wtoi(argv[2])), extraFlags, argv[3], filesFrom(4));
  } else if (mode == L"shell" && argc >= 5) {
    result = RunShell(static_cast<UINT>(_wtoi(argv[2])), argv[3], filesFrom(4));
  } else if (mode == L"stress" && argc >= 7) {
    result = RunStress(argv[2], static_cast<UINT>(_wtoi(argv[3])), _wtoi(argv[4]), _wtoi(argv[5]),
                       filesFrom(6));
  } else {
    result = Usage();
  }
  CoUninitialize();
  return result;
}
