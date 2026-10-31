// pdft-planting-canary.dll (test only; gate.planting). Copies of this DLL are planted next to
// copies of the app's executables under the names of DLLs those might load. Whatever loads one
// gets nothing from it: DllMain writes a marker file and refuses to load (returns FALSE).
//
// The marker is "<own file name>.loaded" (e.g. "version.dll.loaded") in the folder named by the
// PDFT_CANARY_DIR environment variable, and holds the loading program's path (UTF-16).
// Only KERNEL32 is used, so the canary itself loads anywhere.

#include <windows.h>

#include <cwchar>

namespace {

void WriteMarker(HINSTANCE self) {
  wchar_t folder[MAX_PATH] = {};
  const DWORD folderLength = GetEnvironmentVariableW(L"PDFT_CANARY_DIR", folder, MAX_PATH);
  if (folderLength == 0 || folderLength >= MAX_PATH) return;
  wchar_t module[MAX_PATH] = {};
  const DWORD moduleLength = GetModuleFileNameW(self, module, MAX_PATH);
  if (moduleLength == 0 || moduleLength >= MAX_PATH) return;
  const wchar_t* name = module;
  for (const wchar_t* p = module; *p; ++p) {
    if (*p == L'\\' || *p == L'/') name = p + 1;
  }
  wchar_t marker[2 * MAX_PATH + 16] = {};
  if (swprintf_s(marker, L"%ls\\%ls.loaded", folder, name) < 0) return;

  wchar_t program[MAX_PATH] = {};
  const DWORD programLength = GetModuleFileNameW(nullptr, program, MAX_PATH);
  HANDLE file = CreateFileW(marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  if (programLength > 0 && programLength < MAX_PATH) {
    WriteFile(file, program, programLength * sizeof(wchar_t), &written, nullptr);
  }
  CloseHandle(file);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  WriteMarker(instance);
  return FALSE;  // refuse to load: none of this DLL stays in the process that tried
}
