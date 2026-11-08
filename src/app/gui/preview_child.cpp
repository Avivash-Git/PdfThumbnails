#include "preview_child.h"

#include <shlwapi.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "build_folder.h"
#include "context.h"
#include "exit_code.h"
#include "paths.h"
#include "preview_override.h"
#include "preview_protocol.h"
#include "project_config.h"
#include "registration.h"
#include "registry_roots.h"
#include "resource_ids.h"
#include "snapshot.h"

namespace pdft {
namespace {

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

// Past this the child is stuck; the window has given up on it long before.
constexpr DWORD kWatchdogMs = 20000;

// The handler this user's install registered, when it lies where that install keeps its builds
// and is of this major version or later (a 1.x handler has no preview interface).
std::optional<std::filesystem::path> RegisteredHandler() {
  const RegistryRoots roots;
  std::optional<ClassReg> machine;
  std::optional<ClassReg> user;
  if (FAILED(ReadClass(roots, Scope::kMachine, &machine)) ||
      FAILED(ReadClass(roots, Scope::kUser, &user))) {
    return std::nullopt;
  }
  const Scope scope = DecideScope(machine, user);
  if (scope == Scope::kNone) return std::nullopt;
  const ClassReg& active = scope == Scope::kMachine ? *machine : *user;
  if (!active.dllExists || !active.pdfiumExists) return std::nullopt;
  std::filesystem::path root;
  if (FAILED(scope == Scope::kMachine ? GetMachineRoot(&root) : GetUserRoot(&root))) {
    return std::nullopt;
  }
  const std::filesystem::path dll = active.dll;
  if (!IsUnder(dll, root / kBuildsFolderName)) return std::nullopt;
  const std::optional<BuildFolderNameParts> parts =
      ParseBuildFolderName(dll.parent_path().filename().native());
  if (!parts || CompareFileVersions(parts->version, L"2") < 0) return std::nullopt;
  return dll;
}

std::optional<std::filesystem::path> FindHandler() {
  if (std::optional<std::filesystem::path> registered = RegisteredHandler()) return registered;
  if (!kDevInstall) return std::nullopt;
  std::filesystem::path own;
  if (FAILED(GetOwnExe(&own))) return std::nullopt;
  std::filesystem::path beside = own.parent_path() / kHandlerDll;
  const DWORD attributes = GetFileAttributesW(beside.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return std::nullopt;
  }
  return beside;
}

HRESULT SampleStream(UINT id, wil::com_ptr_nothrow<IStream>* stream) noexcept {
  const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
  RETURN_LAST_ERROR_IF_NULL(found);
  const HGLOBAL loaded = LoadResource(nullptr, found);
  RETURN_LAST_ERROR_IF_NULL(loaded);
  const void* data = LockResource(loaded);
  RETURN_LAST_ERROR_IF_NULL(data);
  stream->attach(SHCreateMemStream(static_cast<const BYTE*>(data), SizeofResource(nullptr, found)));
  RETURN_HR_IF_NULL(E_OUTOFMEMORY, stream->get());
  return S_OK;
}

// The bitmap as top-down 32-bit pixels.
HRESULT CopyPixels(HBITMAP bitmap, PreviewFrame* frame) {
  BITMAP info = {};
  RETURN_HR_IF(E_FAIL, !GetObjectW(bitmap, sizeof(info), &info));
  RETURN_HR_IF(E_FAIL, info.bmWidth <= 0 || info.bmHeight <= 0);
  BITMAPINFO format = {};
  format.bmiHeader.biSize = sizeof(format.bmiHeader);
  format.bmiHeader.biWidth = info.bmWidth;
  format.bmiHeader.biHeight = -info.bmHeight;
  format.bmiHeader.biPlanes = 1;
  format.bmiHeader.biBitCount = 32;
  format.bmiHeader.biCompression = BI_RGB;
  frame->pixels.assign(size_t{static_cast<UINT>(info.bmWidth)} * static_cast<UINT>(info.bmHeight),
                       0);
  wil::unique_hdc screen(CreateCompatibleDC(nullptr));
  RETURN_HR_IF_NULL(E_FAIL, screen);
  RETURN_HR_IF(E_FAIL, GetDIBits(screen.get(), bitmap, 0, static_cast<UINT>(info.bmHeight),
                                 frame->pixels.data(), &format, DIB_RGB_COLORS) != info.bmHeight);
  frame->width = static_cast<uint32_t>(info.bmWidth);
  frame->height = static_cast<uint32_t>(info.bmHeight);
  return S_OK;
}

HRESULT RenderSample(IClassFactory* factory, UINT id, int zoom, int edge, PreviewFrame* frame) {
  wil::com_ptr_nothrow<IStream> stream;
  RETURN_IF_FAILED(SampleStream(id, std::addressof(stream)));
  wil::com_ptr_nothrow<IInitializeWithStream> init;
  RETURN_IF_FAILED(factory->CreateInstance(nullptr, kIidInitializeWithStream,
                                           reinterpret_cast<void**>(init.put())));
  // A handler without this interface would render at the saved zoom, not the one asked for.
  wil::com_ptr_nothrow<IPdfThumbnailsPreviewOptions> options;
  RETURN_IF_FAILED(init->QueryInterface(kPreviewIid, reinterpret_cast<void**>(options.put())));
  RETURN_IF_FAILED(options->SetZoom(zoom));
  RETURN_IF_FAILED(init->Initialize(stream.get(), STGM_READ));
  wil::com_ptr_nothrow<IThumbnailProvider> provider;
  RETURN_IF_FAILED(init->QueryInterface(kThumbSlotIid, reinterpret_cast<void**>(provider.put())));
  wil::unique_hbitmap bitmap;
  WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
  RETURN_IF_FAILED(provider->GetThumbnail(static_cast<UINT>(edge), bitmap.put(), &alpha));
  return CopyPixels(bitmap.get(), frame);
}

bool WriteAll(HANDLE output, const std::string& bytes) {
  DWORD written = 0;
  return WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
         written == bytes.size();
}

}  // namespace

int RenderPreviewChild(int zoom, int edge) noexcept try {
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  std::thread([] {
    Sleep(kWatchdogMs);
    TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
  }).detach();

  ProcessContext context;
  if (FAILED(GetProcessContext(ContextProbe::kNone, &context))) {
    return static_cast<int>(ExitCode::kFailed);
  }
  if (context.elevated) return static_cast<int>(ExitCode::kWrongContext);
  const std::optional<std::filesystem::path> dll = FindHandler();
  if (!dll) return static_cast<int>(ExitCode::kNotInstalled);

  const HMODULE module = LoadLibraryExW(
      dll->c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) return static_cast<int>(ExitCode::kFailed);
  const auto getClassObject = reinterpret_cast<GetClassObjectFn>(
      reinterpret_cast<void*>(GetProcAddress(module, "DllGetClassObject")));
  wil::com_ptr_nothrow<IClassFactory> factory;
  if (!getClassObject || FAILED(getClassObject(kHandlerClsid, __uuidof(IClassFactory),
                                               reinterpret_cast<void**>(factory.put())))) {
    return static_cast<int>(ExitCode::kFailed);
  }

  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!WriteAll(output, EncodePreviewHeader())) return static_cast<int>(ExitCode::kFailed);
  constexpr UINT kSamples[kPreviewSampleCount] = {IDR_SAMPLE_DOCUMENT, IDR_SAMPLE_SLIDE};
  for (uint32_t index = 0; index < kPreviewSampleCount; ++index) {
    PreviewFrame frame;
    frame.sample = static_cast<PreviewSample>(index);
    frame.hr = RenderSample(factory.get(), kSamples[index], zoom, edge, &frame);
    if (FAILED(frame.hr)) {
      frame.width = frame.height = 0;
      frame.pixels.clear();
    }
    if (!WriteAll(output, EncodePreviewFrame(frame))) return static_cast<int>(ExitCode::kFailed);
  }
  return static_cast<int>(ExitCode::kOk);
} catch (...) {
  return static_cast<int>(ExitCode::kFailed);
}

}  // namespace pdft
