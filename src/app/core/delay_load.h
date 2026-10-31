// System DLLs the app delay-loads (cmake/Hardening.cmake, pdft_delay_load): checking that one can
// be loaded before the first call into it.
//
// A delay-loaded function is bound at its first call. If the DLL or the function cannot be found
// there, the delay-load helper raises a structured exception, which no C++ handler catches: the
// process would end without a log line. Calling EnsureDelayLoaded first turns that case into an
// HRESULT the caller can report ("the Code Integrity log could not be read", ...).

#pragma once

#include <windows.h>

namespace pdft {

// Binds every delay-loaded import of |dll| ("wevtapi.dll", compared ignoring case). S_OK when
// they are all bound, also when this program links the DLL normally and it is already loaded;
// otherwise the loader's error (the DLL or a function missing).
[[nodiscard]] HRESULT EnsureDelayLoaded(const char* dll) noexcept;

}  // namespace pdft
