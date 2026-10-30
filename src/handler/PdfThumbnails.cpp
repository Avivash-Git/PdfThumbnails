// PdfThumbnails.dll: Windows Explorer thumbnail provider that shows the first
// page of PDF files, rendered with PDFium.
//
// Explorer creates this COM object in its isolated thumbnail process, hands it
// the file as an IStream (IInitializeWithStream) and asks for a bitmap
// (IThumbnailProvider). PDFium is loaded from this DLL's own folder under a
// private file name. The settings app registers the class (until it ships, the
// 1.x PowerShell manager, PdfThumbnails.ps1, does), so there is no
// DllRegisterServer here. The zoom comes from HKCU\Software\PdfThumbnails, with
// the 1.x settings.ini as fallback (see ReadZoomPercent); a preview can set its
// own through a private interface (preview_override.h).

#include <windows.h>

#include <propsys.h>
#include <shlobj.h>
#include <thumbcache.h>

#include <atomic>
#include <new>

#include "crop_math.h"
#include "fpdf_formfill.h"
#include "fpdf_progressive.h"
#include "fpdfview.h"
#include "preview_override.h"
#include "project_config.h"
#include "zoom.h"

#ifndef WTS_E_FAILEDEXTRACTION
#define WTS_E_FAILEDEXTRACTION ((HRESULT)0x8004B200L)
#endif
#ifndef WTS_E_EXTRACTIONTIMEDOUT
#define WTS_E_EXTRACTIONTIMEDOUT ((HRESULT)0x8004B201L)
#endif

namespace {

// Names and IDs come from cmake/ProjectInfo.cmake (project_config.h). The CLSID never
// changes; the IIDs are spelled out there so the build does not depend on uuid.lib knowing them.
constexpr CLSID kClsidPdfThumbnails = pdft::kHandlerClsid;
constexpr IID kIidThumbnailProvider = pdft::kThumbSlotIid;
constexpr IID kIidInitializeWithStream = pdft::kIidInitializeWithStream;
constexpr IID kIidPreviewOptions = pdft::kPreviewIid;
constexpr GUID kFolderIdLocalAppData = pdft::kFolderIdLocalAppData;

// Private name, so another extension's LoadLibrary("pdfium.dll") in the shared
// thumbnail process can never bind to this copy, or this code to theirs.
using pdft::kPdfiumDll;
// Explorer does not ask for more than 2560 px; cap it anyway (crop_math.h).
using pdft::kMaxEdge;
// The whole file is read into memory first; bigger PDFs keep the plain icon.
constexpr ULONGLONG kMaxFileBytes = 256ull << 20;
// Time limits per thumbnail. Slower files keep the plain icon. Opening the
// document and parsing page 1 cannot be interrupted, so those may overrun.
constexpr ULONGLONG kReadBudgetMs = 8000;    // reading the file
constexpr ULONGLONG kLockWaitMs = 15000;     // from the start, waiting for other PDFs
constexpr ULONGLONG kRenderBudgetMs = 8000;  // from getting PDFium to the finished page
// Draw annotations too (stamps, highlights); form fields come from FPDF_FFLDraw.
constexpr int kRenderFlags = FPDF_ANNOT;
// Magnification in percent, 100 to 200 (zoom.h; see ReadZoomPercent).
using pdft::kMinZoom;

HMODULE g_module = nullptr;
LONG g_serverRefs = 0;  // live objects + IClassFactory::LockServer locks

// ---- PDFium -----------------------------------------------------------------

// One member per function in pdfium_functions.inc, named by its second argument.
struct Pdfium {
#define PDFT_FN(name, member) decltype(&name) member;
#include "pdfium_functions.inc"
#undef PDFT_FN
};

Pdfium g_pdfium;
HMODULE g_pdfiumModule = nullptr;
INIT_ONCE g_pdfiumOnce = INIT_ONCE_STATIC_INIT;
// PDFium is not thread-safe: one call at a time, process-wide.
SRWLOCK g_pdfiumLock = SRWLOCK_INIT;
std::atomic<DWORD> g_pdfiumOwner{0};  // thread holding g_pdfiumLock, 0 if none

template <typename Fn>
bool Resolve(HMODULE dll, const char* name, Fn* fn) {
  *fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
  return *fn != nullptr;
}

// Binds every function in pdfium_functions.inc, in list order; false at the first one
// |dll| lacks.
bool ResolveAll(HMODULE dll, Pdfium& p) {
#define PDFT_FN(name, member) \
  if (!Resolve(dll, #name, &p.member)) return false;
#include "pdfium_functions.inc"
#undef PDFT_FN
  return true;
}

bool IsInModule(HMODULE module, const void* address) {
  HMODULE owner = nullptr;
  return module &&
         GetModuleHandleExW(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             static_cast<LPCWSTR>(address), &owner) &&
         owner == module;
}

// If PDFium (or this DLL) faults while holding the PDFium lock and the host
// swallows the exception, the lock and PDFium's state would stay broken for the
// life of the process. End the process instead; Explorer starts a fresh one.
LONG CALLBACK FailFastOnPdfiumFault(EXCEPTION_POINTERS* info) {
  if (g_pdfiumOwner.load() != GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  const DWORD code = record->ExceptionCode;
  const bool crash = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                     code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_BREAKPOINT ||
                     code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW;
  const bool ours = IsInModule(g_pdfiumModule, record->ExceptionAddress) ||
                    IsInModule(g_module, record->ExceptionAddress);
  // 0xE0000008 is PDFium's out-of-memory exception, raised from kernelbase.
  if (code == 0xE0000008 || (crash && ours))
    RaiseFailFastException(info->ExceptionRecord, info->ContextRecord, 0);
  return EXCEPTION_CONTINUE_SEARCH;
}

BOOL CALLBACK LoadPdfium(PINIT_ONCE, PVOID, PVOID*) {
  // Load PDFium from our own folder by full path, never via the search path.
  wchar_t path[MAX_PATH];
  DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return FALSE;
  wchar_t* name = wcsrchr(path, L'\\');
  if (!name) return FALSE;
  size_t room = MAX_PATH - static_cast<size_t>(name + 1 - path);
  if (room < ARRAYSIZE(kPdfiumDll)) return FALSE;
  wcscpy_s(name + 1, room, kPdfiumDll);

  HMODULE dll = LoadLibraryExW(path, nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) return FALSE;
  Pdfium& p = g_pdfium;
  if (!ResolveAll(dll, p)) {
    FreeLibrary(dll);
    return FALSE;
  }
  g_pdfiumModule = dll;

  // PDFium cannot be shut down and initialised again (and its init starts
  // GDI+), so pin this DLL, which keeps its reference on PDFium, for the rest
  // of the process. Nothing ever calls FPDF_DestroyLibrary.
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(&g_module), &self);
  AddVectoredExceptionHandler(1, FailFastOnPdfiumFault);
  FPDF_LIBRARY_CONFIG config = {};
  config.version = 2;  // no user font paths: PDFium maps fonts through GDI
  p.InitLibraryWithConfig(&config);
  return TRUE;
}

// Waits for the PDFium lock until |deadline|. Re-entry from the thread that
// already holds it fails instead of deadlocking.
bool AcquirePdfiumLock(ULONGLONG deadline) {
  if (g_pdfiumOwner.load() == GetCurrentThreadId()) return false;
  while (!TryAcquireSRWLockExclusive(&g_pdfiumLock)) {
    if (GetTickCount64() >= deadline) return false;
    Sleep(10);
  }
  g_pdfiumOwner.store(GetCurrentThreadId());
  return true;
}

void ReleasePdfiumLock() {
  g_pdfiumOwner.store(0);
  ReleaseSRWLockExclusive(&g_pdfiumLock);
}

// True in elevated processes (and services): like per-user COM classes, this
// handler never parses PDFs there. Explorer and its thumbnail process run at
// Medium integrity.
bool IsAboveMediumIntegrity() {
  static const bool above = [] {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
    alignas(TOKEN_MANDATORY_LABEL)
        BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    bool result = true;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
      PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
      result = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) >
               SECURITY_MANDATORY_MEDIUM_RID;
    }
    CloseHandle(token);
    return result;
  }();
  return above;
}

// The zoom in the 1.x settings file, read exactly as 1.1 reads it:
//   %LOCALAPPDATA%\Programs\PdfThumbnails\settings.ini   [Thumbnails] Zoom=<percent>
// PDFTHUMBNAILS_SETTINGS can name another file, which the tests use. 100 without one.
int ReadLegacyZoomPercent() {
  wchar_t path[MAX_PATH];
  DWORD length = GetEnvironmentVariableW(pdft::kLegacySettingsEnv, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(kFolderIdLocalAppData, 0, nullptr, &folder))) return kMinZoom;
    bool fits = wcscpy_s(path, MAX_PATH, folder) == 0 &&
                wcscat_s(path, MAX_PATH, pdft::kLegacySettingsIni) == 0;
    CoTaskMemFree(folder);
    if (!fits) return kMinZoom;
  }
  UINT zoom = GetPrivateProfileIntW(L"Thumbnails", L"Zoom", kMinZoom, path);
  return pdft::ClampZoom(zoom);
}

// The user's magnification (100, the default, shows the whole page), read fresh
// for every thumbnail. The settings app saves it as a REG_DWORD:
//   HKCU\Software\PdfThumbnails   Zoom=<percent>
// When that value is missing, has another type (REG_SZ, REG_QWORD, ...) or cannot
// be read, the 1.x settings file decides.
int ReadZoomPercent() {
  DWORD zoom = 0;
  DWORD size = sizeof(zoom);
  if (RegGetValueW(HKEY_CURRENT_USER, pdft::kRegKey, pdft::kZoomValueName, RRF_RT_REG_DWORD,
                   nullptr, &zoom, &size) == ERROR_SUCCESS &&
      size == sizeof(zoom)) {
    return pdft::ClampZoom(zoom);
  }
  return ReadLegacyZoomPercent();
}

// The whole PDF in memory, so PDFium never calls back into COM.
class FileBuffer {
 public:
  FileBuffer() = default;
  FileBuffer(const FileBuffer&) = delete;
  FileBuffer& operator=(const FileBuffer&) = delete;
  ~FileBuffer() {
    if (data_) VirtualFree(data_, 0, MEM_RELEASE);
  }

  HRESULT ReadAll(IStream* stream, ULONGLONG deadline) {
    ULONGLONG size = 0;
    STATSTG stat = {};
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
      size = stat.cbSize.QuadPart;
    } else {
      LARGE_INTEGER zero = {};
      ULARGE_INTEGER end = {};
      if (FAILED(stream->Seek(zero, STREAM_SEEK_END, &end))) return WTS_E_FAILEDEXTRACTION;
      size = end.QuadPart;
    }
    if (size == 0 || size > kMaxFileBytes) return WTS_E_FAILEDEXTRACTION;

    data_ = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!data_) return E_OUTOFMEMORY;
    LARGE_INTEGER start = {};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return WTS_E_FAILEDEXTRACTION;
    while (size_ < size) {
      if (GetTickCount64() >= deadline) return WTS_E_EXTRACTIONTIMEDOUT;
      ULONGLONG left = size - size_;
      ULONG chunk = left < (1u << 20) ? static_cast<ULONG>(left) : (1u << 20);
      ULONG read = 0;
      // Fails if the file shrank, or Explorer let go of it for another program.
      if (FAILED(stream->Read(data_ + size_, chunk, &read)) || read == 0)
        return WTS_E_FAILEDEXTRACTION;
      size_ += read;
    }
    return S_OK;
  }

  const unsigned char* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  unsigned char* data_ = nullptr;
  size_t size_ = 0;
};

FPDF_BOOL NeedToPauseNow(IFSDK_PAUSE* pause) {
  return GetTickCount64() >= *static_cast<const ULONGLONG*>(pause->user);
}

// Draws |page| (|size| in points) to fit an |edge| x |edge| square. Above 100%
// |zoom| crops the page's longer side evenly (top and bottom of a portrait page)
// so the rest fills more of the square, never cropping past a square
// (ComputeCrop in crop_math.h). Caller holds the PDFium lock.
HRESULT RenderPage(FPDF_PAGE page, FPDF_FORMHANDLE form, FS_SIZEF size, UINT edge, int zoom,
                   ULONGLONG deadline, HBITMAP* result) {
  const Pdfium& p = g_pdfium;
  // The bitmap, and the whole page at the chosen scale placed so the crop is centred.
  const pdft::CropGeometry crop = pdft::ComputeCrop(size.width, size.height, edge, zoom);
  const int width = crop.bmpW;
  const int height = crop.bmpH;

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down, the row order PDFium writes
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!dib) return E_OUTOFMEMORY;

  int status = FPDF_RENDER_FAILED;
  if (FPDF_BITMAP bitmap = p.BitmapCreateEx(width, height, FPDFBitmap_BGRx, pixels, width * 4)) {
    p.BitmapFillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);  // white paper
    IFSDK_PAUSE pause = {};
    pause.version = 1;
    pause.NeedToPauseNow = NeedToPauseNow;
    pause.user = &deadline;
    // PDFium only pauses once the deadline has passed, so anything other than
    // DONE means the page failed or was too slow.
    status = p.RenderPageBitmapStart(bitmap, page, crop.pageLeft, crop.pageTop, crop.pageW,
                                     crop.pageH, 0, kRenderFlags, &pause);
    if (status == FPDF_RENDER_DONE && form) {
      p.FFLDraw(form, bitmap, page, crop.pageLeft, crop.pageTop, crop.pageW, crop.pageH, 0,
                kRenderFlags);
    }
    p.RenderPageClose(page);
    p.BitmapDestroy(bitmap);  // leaves the DIB alone
  }
  if (status != FPDF_RENDER_DONE) {
    DeleteObject(dib);
    return status == FPDF_RENDER_TOBECONTINUED ? WTS_E_EXTRACTIONTIMEDOUT : WTS_E_FAILEDEXTRACTION;
  }

  // BGRx leaves the fourth byte undefined; make every pixel opaque.
  auto* pixel = static_cast<DWORD*>(pixels);
  for (size_t i = 0, count = static_cast<size_t>(width) * height; i < count; ++i)
    pixel[i] |= 0xFF000000;
  *result = dib;
  return S_OK;
}

// Opens the PDF in |file| and draws its first page. Caller holds the PDFium lock.
HRESULT RenderFirstPage(const FileBuffer& file, UINT edge, int zoom, ULONGLONG deadline,
                        HBITMAP* result) {
  const Pdfium& p = g_pdfium;
  // Fails for non-PDFs, broken files and documents that need a password to
  // open; PDFs restricted only by an owner password open and render normally.
  FPDF_DOCUMENT document = p.LoadMemDocument64(file.data(), file.size(), nullptr);
  if (!document) return WTS_E_FAILEDEXTRACTION;

  // Opening and parsing cannot be interrupted, so check the clock between steps.
  auto expired = [deadline] { return GetTickCount64() >= deadline; };
  HRESULT hr = WTS_E_FAILEDEXTRACTION;
  FS_SIZEF size = {};
  if (expired()) {
    hr = WTS_E_EXTRACTIONTIMEDOUT;
  } else if (p.GetFormType(document) != FORMTYPE_XFA_FULL &&  // see below
             p.GetPageCount(document) > 0 && p.GetPageSizeByIndexF(document, 0, &size) &&
             size.width >= 1 && size.height >= 1 && size.width < 1e6f && size.height < 1e6f) {
    // (Dynamic XFA forms keep the icon: their page 1 is only a "Please wait..."
    // placeholder, and this PDFium build cannot lay out the real form.)
    // Without a form environment PDFium leaves out filled-in form fields,
    // checkboxes and visible signatures. |formInfo| must outlive |form|.
    FPDF_FORMFILLINFO formInfo = {};
    formInfo.version = 1;
    FPDF_FORMHANDLE form = p.InitFormFillEnvironment(document, &formInfo);
    if (FPDF_PAGE page = p.LoadPage(document, 0)) {
      if (expired()) {
        hr = WTS_E_EXTRACTIONTIMEDOUT;
      } else {
        if (form) p.FormOnAfterLoadPage(page, form);
        hr = expired() ? WTS_E_EXTRACTIONTIMEDOUT
                       : RenderPage(page, form, size, edge, zoom, deadline, result);
        if (form) p.FormOnBeforeClosePage(page, form);
      }
      p.ClosePage(page);
    }
    if (form) p.ExitFormFillEnvironment(form);
  }
  p.CloseDocument(document);
  return hr;
}

// ---- COM --------------------------------------------------------------------

// Explorer uses IInitializeWithStream and IThumbnailProvider. Only a preview that
// asks for the private IID gets IPdfThumbnailsPreviewOptions.
class PdfThumbnailProvider final : public IInitializeWithStream,
                                   public IThumbnailProvider,
                                   public pdft::IPdfThumbnailsPreviewOptions {
 public:
  PdfThumbnailProvider() { InterlockedIncrement(&g_serverRefs); }

  // IUnknown. IID_IUnknown always gives the IInitializeWithStream pointer (COM identity).
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, kIidInitializeWithStream)) {
      *object = static_cast<IInitializeWithStream*>(this);
    } else if (IsEqualIID(iid, kIidThumbnailProvider)) {
      *object = static_cast<IThumbnailProvider*>(this);
    } else if (IsEqualIID(iid, kIidPreviewOptions)) {
      *object = static_cast<pdft::IPdfThumbnailsPreviewOptions*>(this);
    } else {
      *object = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    LONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  // IInitializeWithStream. The file is only ever read, never reopened by path.
  STDMETHODIMP Initialize(IStream* stream, DWORD /*mode*/) override {
    if (!stream) return E_INVALIDARG;
    if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    stream_ = stream;
    stream_->AddRef();
    return S_OK;
  }

  // IThumbnailProvider
  STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
    if (!bitmap || !alpha) return E_POINTER;
    *bitmap = nullptr;
    *alpha = WTSAT_UNKNOWN;
    if (!stream_) return E_UNEXPECTED;
    if (cx == 0) return E_INVALIDARG;
    if (IsAboveMediumIntegrity()) return WTS_E_FAILEDEXTRACTION;
    if (!InitOnceExecuteOnce(&g_pdfiumOnce, LoadPdfium, nullptr, nullptr))
      return WTS_E_FAILEDEXTRACTION;

    const ULONGLONG start = GetTickCount64();
    // Read the file before taking the PDFium lock, so no COM call (which may
    // pump messages on this STA thread) happens while the lock is held.
    FileBuffer file;
    HRESULT hr = file.ReadAll(stream_, start + kReadBudgetMs);
    if (FAILED(hr)) return hr;
    const int zoom = previewZoom_ ? previewZoom_ : ReadZoomPercent();
    if (!AcquirePdfiumLock(start + kLockWaitMs)) return WTS_E_EXTRACTIONTIMEDOUT;
    hr = RenderFirstPage(file, cx < kMaxEdge ? cx : kMaxEdge, zoom,
                         GetTickCount64() + kRenderBudgetMs, bitmap);
    ReleasePdfiumLock();
    if (SUCCEEDED(hr)) *alpha = WTSAT_RGB;
    return hr;
  }

  // IPdfThumbnailsPreviewOptions: this object ignores the saved zoom from now on.
  STDMETHODIMP SetZoom(int percent) override {
    previewZoom_ = pdft::ClampZoom(percent);
    return S_OK;
  }

 private:
  ~PdfThumbnailProvider() {
    if (stream_) stream_->Release();
    InterlockedDecrement(&g_serverRefs);
  }

  LONG refs_ = 1;
  IStream* stream_ = nullptr;
  int previewZoom_ = 0;  // 0 until SetZoom; then kMinZoom..kMaxZoom
};

class ClassFactory final : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IClassFactory)) {
      *object = static_cast<IClassFactory*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  // The factory is a static object that lives as long as the module.
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* provider = new (std::nothrow) PdfThumbnailProvider();
    if (!provider) return E_OUTOFMEMORY;
    HRESULT hr = provider->QueryInterface(iid, object);
    provider->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock)
      InterlockedIncrement(&g_serverRefs);
    else
      InterlockedDecrement(&g_serverRefs);
    return S_OK;
  }
};

ClassFactory g_factory;

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) g_module = instance;
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (!IsEqualCLSID(clsid, kClsidPdfThumbnails)) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(iid, object);
}

STDAPI DllCanUnloadNow() { return g_serverRefs == 0 ? S_OK : S_FALSE; }
