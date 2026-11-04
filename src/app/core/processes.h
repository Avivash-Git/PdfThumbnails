// Other processes: which ones have a file open or loaded (Restart Manager), ending one of them
// when it is safe, and this process's parent.
//
// Windows' thumbnail process (dllhost.exe, the COM surrogate) keeps a thumbnail handler's DLLs
// loaded for as long as it lives. Before an install replaces a build, the processes of the
// user's own session that hold the OLD build's DLLs are ended, so the new build is loaded the
// next time Explorer asks for a thumbnail. Only processes that pass every check are ended: the
// image on an allow list (normally just %SystemRoot%\System32\dllhost.exe), the same session,
// the same user, and still the very process that was found (same process id and start time).
// Rebuilding the thumbnail cache allows File Explorer too (GetRebuildImages), and starts it
// again where Windows does not.

#pragma once

#include <windows.h>

#include <RestartManager.h>
#include <wil/resource.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pdft {

// A process that has one of the files open or loaded.
struct Holder {
  DWORD pid = 0;
  FILETIME start = {};  // when it started, to tell it from a later process with the same id
  DWORD session = 0;    // its Terminal Services session
  RM_APP_TYPE type = RmUnknownApp;
  std::wstring app;    // Restart Manager's name for it (a service name, a window title, ...)
  std::wstring image;  // its executable's full path; empty when it cannot be read (another
                       // user's or an elevated process)
};

// Every process that has one of |files| open or loaded, from Restart Manager. A process found
// but gone, or replaced by another one with the same id, by the time it is looked at is left out.
// An empty list of files gives no holders.
[[nodiscard]] HRESULT FindHolders(std::span<const std::filesystem::path> files,
                                  std::vector<Holder>* holders) noexcept;

// The image an install may end a holder of: <System folder>\dllhost.exe, the thumbnail process.
[[nodiscard]] HRESULT SurrogateImagePath(std::wstring* image) noexcept;

// Whether |a| and |b| are the same path (pure): whole paths, ignoring case, after "." and ".."
// are resolved; a relative |a| never matches. No file system access, so a link or a short (8.3)
// name is not the same as where it leads.
bool SameImagePath(std::wstring_view a, std::wstring_view b) noexcept;

// What TerminateOwnSessionHolder knows about a process when it decides.
struct HolderFacts {
  DWORD ownPid = 0;
  DWORD ownSession = 0;
  DWORD session = 0;      // the process's session now
  FILETIME start = {};    // its start time now
  std::wstring image;     // its image now
  bool sameUser = false;  // its token's user is this process's user
};

// Why a holder is not ended.
enum class HolderRefusal {
  kNone,          // it may be ended
  kSelf,          // it is this process
  kReplaced,      // its start time changed: the process found is gone, the id reused
  kOtherSession,  // another session (another signed-in user, or a service)
  kOtherUser,     // another account's process, or its user cannot be read
  kNotAllowed,    // its image is not on the allow list (or was not read)
};

// The decision for |found| (pure): the checks in the order of the enum. Images compare as whole
// paths, ignoring case, after "." and ".." are resolved.
HolderRefusal JudgeHolder(const Holder& found, const HolderFacts& now,
                          std::span<const std::wstring_view> allowedImages) noexcept;

// Ends the process |holder| (FindHolders) after reading its facts again and judging them
// (JudgeHolder), and waits up to 5 s for it to go. S_FALSE when it is gone already (or replaced
// by another process with its id); E_ACCESSDENIED when a check refuses it, and it is left alone.
[[nodiscard]] HRESULT TerminateOwnSessionHolder(
    const Holder& holder, std::span<const std::wstring_view> allowedImages) noexcept;

// ---- Rebuilding the thumbnail cache (rebuild.h) -------------------------------------------

// The images the thumbnail-cache rebuild may end: File Explorer and the COM surrogate (the
// thumbnail process). |surrogateWow64| is empty where Windows has no WOW64.
struct RebuildImages {
  std::wstring explorer;        // <System Windows folder>\explorer.exe
  std::wstring surrogate;       // <System folder>\dllhost.exe
  std::wstring surrogateWow64;  // <SysWOW64>\dllhost.exe
};
[[nodiscard]] HRESULT GetRebuildImages(RebuildImages* images) noexcept;

// Whether a process of |image| (a whole path, compared as SameImagePath does) runs in this
// process's session as this process's user and started after |since| (UTC), from a process
// snapshot. Processes that cannot be read do not count.
bool OwnProcessStartedSince(std::wstring_view image, const FILETIME& since) noexcept;

// Starts <Windows>\explorer.exe as this process's user with a fresh environment from the user's
// profile (not this process's: a terminal's variables must not reach what the user starts from
// File Explorer), nothing inherited, in System32, outside this process's job when it is in one.
// *jobForbids is true when the job does not let it leave (nothing started; the call fails with
// HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)). Does not wait for it. Never call it below an app
// package or when elevated.
[[nodiscard]] HRESULT StartExplorerFreshEnvironment(bool* jobForbids) noexcept;

// ---- This process's parent ----------------------------------------------------------------

// This process's parent: the process id its creator had, provided that process still exists
// and was created before this one (so the id was not reused). Fails with
// HRESULT_FROM_WIN32(ERROR_NOT_FOUND) otherwise.
[[nodiscard]] HRESULT GetParentProcessId(DWORD* parent) noexcept;

// The same parent, with the handle (PROCESS_QUERY_LIMITED_INFORMATION) its creation time was
// checked through. While |parent| stays open, |parentId| cannot pass to another process, so
// whatever is checked against the id meanwhile is checked against this parent.
[[nodiscard]] HRESULT OpenParentProcess(DWORD* parentId, wil::unique_handle* parent) noexcept;

}  // namespace pdft
