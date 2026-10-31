// Process-wide hardening for the app's executables. HardenProcess() is the first statement of
// wWinMain and wmain. From then on, a DLL that is not a KnownDLL loads from System32 only, so a
// DLL planted next to the executable (or in the current directory) is never picked up.
//
// It has to come before anything that loads a DLL by name. The executables import only KnownDLLs
// statically; everything else is delay-loaded (cmake/Hardening.cmake) and loads on its first
// call through the default search path, which this restricts. So no static initialiser anywhere
// in the app may call a delay-loaded function or do other real work: globals are constants or
// trivially initialised.

#pragma once

#include <windows.h>

namespace pdft {

// Applies, in this order, every step even when an earlier one fails:
//   SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)  LoadLibrary(Ex) by name, and with it
//                       the delay-load helper, search System32 only
//   SetDllDirectoryW(L"")  the current directory leaves the classic search order too
//   SetSearchPathMode(safe mode, permanent)  SearchPathW looks in the current directory last
//   HeapSetInformation(HeapEnableTerminationOnCorruption)
//   SetProcessMitigationPolicy(ProcessImageLoadPolicy)  no images from remote shares or with a
//                       Low mandatory label; System32 before the executable's folder
// Returns S_OK, or the first failure. The caller must then stop: the search path may still
// include the executable's folder.
[[nodiscard]] HRESULT HardenProcess() noexcept;

}  // namespace pdft
