#include "delay_load.h"

#include <delayimp.h>

// The linker's symbol for the start of this program's own image.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace pdft {
namespace {

char AsciiLower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool AsciiEqualsIgnoringCase(const char* a, const char* b) noexcept {
  for (; *a != '\0' && *b != '\0'; ++a, ++b) {
    if (AsciiLower(*a) != AsciiLower(*b)) return false;
  }
  return *a == *b;
}

// The name of |dll| as this program's delay-import table spells it, or nullptr when the program
// does not delay-load it. The delay-load helper compares names case-sensitively, so the caller's
// spelling is first matched against the table here, ignoring case. Reads only this program's
// own mapped image.
const char* DelayImportName(const char* dll) noexcept {
  const auto* base = reinterpret_cast<const BYTE*>(&__ImageBase);
  const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
  const IMAGE_DATA_DIRECTORY& directory =
      headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
  if (directory.VirtualAddress == 0 || directory.Size == 0) return nullptr;
  for (auto* descriptor =
           reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + directory.VirtualAddress);
       descriptor->DllNameRVA != 0; ++descriptor) {
    const auto* name = reinterpret_cast<const char*>(base + descriptor->DllNameRVA);
    if (AsciiEqualsIgnoringCase(name, dll)) return name;
  }
  return nullptr;
}

}  // namespace

HRESULT EnsureDelayLoaded(const char* dll) noexcept {
  if (dll == nullptr) return E_INVALIDARG;
  if (const char* name = DelayImportName(dll)) {
    // Returns an HRESULT instead of raising the exception a failed delay-load call would.
    return __HrLoadAllImportsForDll(name);
  }
  // Not delay-loaded by this program: then the DLL is linked normally and was loaded with the
  // program, or it is not used at all.
  return GetModuleHandleA(dll) != nullptr ? S_OK : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

}  // namespace pdft
