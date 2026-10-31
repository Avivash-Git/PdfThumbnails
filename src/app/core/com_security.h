// COM for the app's executables: the main thread's apartment and the process-wide COM security.

#pragma once

#include <windows.h>

namespace pdft {

// Who may call into COM objects this process exposes (callbacks and event sinks it hands to
// Explorer, Task Scheduler or WMI): the process's own user (PS, "principal self") and SYSTEM, with
// COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL (0x3). Nobody else, never anonymous or remote
// callers, which a NULL descriptor would let in. COM requires an owner and a group:
// Administrators.
inline constexpr wchar_t kComAccessSddl[] = L"O:BAG:BAD:(A;;0x3;;;PS)(A;;0x3;;;SY)";

// Initializes COM on the calling thread as a single-threaded apartment (without OLE1 DDE), then
// sets the process-wide COM security with CoInitializeSecurity: kComAccessSddl as an absolute
// security descriptor, packet privacy (calls are authenticated and encrypted), impersonation
// level "impersonate" (WMI and Task Scheduler need it), no further capabilities.
// Call once, on the main thread, after HardenProcess() and before any other COM call
// (CoInitializeSecurity fails once COM has been used). On success the thread stays initialized
// and the caller pairs this with CoUninitialize(); on failure nothing is left initialized.
[[nodiscard]] HRESULT InitializeComSecurity() noexcept;

}  // namespace pdft
