// Smart App Control: whether it is on, how often Code Integrity refused to load the installed
// build's DLLs, and the verdict the status shows.
//
// Smart App Control may refuse to load the handler's unsigned DLLs into Explorer's thumbnail
// process, but it does not refuse every unsigned DLL (builds made on the same PC usually load).
// So the verdict rests on evidence only: an Explorer test that passed (not blocking), an Explorer
// test that failed with a "load blocked" error, or block events (Code Integrity event 3077) that
// name a file in the installed build folder. Without evidence either way it says only that the
// feature is on.
//
// The state is the undocumented value VerifiedAndReputablePolicyState under
// HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy: 0 off, 1 on, 2 evaluation. It is read through
// RegistryRoots, so tests can plant any value; any other value is shown as "unknown (<value>)".
//
// How a check uses this, in order (the 1.x rules):
//   1. ReadSacState.
//   2. The Explorer test, when it runs, gives the probe HRESULT.
//   3. If ShouldCountBlocks: CountCiBlocks(<the registered DLL's folder>, kSacBlockDays).
//      If ShouldRecountBlocks says so, wait kSacRecountDelayMs and count again,
//      keeping the second answer: a block the Explorer test itself caused reaches the log within
//      about a second.
//   4. DecideSac with the state, the probe (std::nullopt when the test did not run) and the count
//      (std::nullopt when step 3 did not count).

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "registry_roots.h"

namespace pdft {

// Where the state lives, relative to the machine-wide root.
inline constexpr wchar_t kSacPolicyKey[] = L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy";
inline constexpr wchar_t kSacPolicyValue[] = L"VerifiedAndReputablePolicyState";

enum class SacState {
  kOff,         // 0
  kOn,          // 1
  kEvaluation,  // 2
  kAbsent,      // no value: a Windows without Smart App Control
  kUnknown,     // any other value, or a value that is not a REG_DWORD
};

struct SacReading {
  SacState state = SacState::kAbsent;
  // For "unknown (<value>)": the REG_DWORD as stored; std::nullopt when the value is missing or
  // is not a 4-byte REG_DWORD (then |type| says what it is).
  std::optional<DWORD> value;
  DWORD type = REG_NONE;  // the value's registry type; REG_NONE when it is missing
};

// The state that |value| (as ReadRegistryValue returns it) stands for (pure).
SacReading ClassifySacValue(const std::optional<RegistryValue>& value) noexcept;

// Reads the state from roots.machine. Read-only; a missing key or value is kAbsent.
[[nodiscard]] HRESULT ReadSacState(const RegistryRoots& roots, SacReading* reading) noexcept;

// What "unknown (<value>)" shows (pure): the number for a REG_DWORD, "type <n>" for a value of
// another type, "" when there is no value. May throw std::bad_alloc.
std::wstring SacValueText(const SacReading& reading);

// ---- Code Integrity block events ----------------------------------------------------------

// How far back blocks count, and how long to wait before the second count.
inline constexpr int kSacBlockDays = 7;
inline constexpr DWORD kSacRecountDelayMs = 1500;

// The answer of CountCiBlocks. |readable| is false when the build folder could not be found or
// the Code Integrity log could not be opened or read to the end (|error| says why); |count| then
// means nothing. 1.x took an unreadable log for "no blocks"; this keeps the two apart. An event
// that cannot be rendered is passed over and does not make the log unreadable, but a log in which
// no event could be rendered is unreadable (RenderOutcome).
struct BlockCount {
  bool readable = false;
  int count = 0;
  HRESULT error = S_OK;
};

// The folder |folder| as Code Integrity names the files in it: its final path on its NT device
// (\Device\HarddiskVolumeN\...), with links and junctions followed, short names expanded and no
// separator at the end. Only an absolute drive path below the drive's root ("X:\...") is opened;
// anything else (a share, a \\?\ or \\.\ path, a relative one) fails with E_INVALIDARG. Fails
// with the error of opening it for a folder that does not exist.
[[nodiscard]] HRESULT DeviceFolderPath(const std::filesystem::path& folder,
                                       std::wstring* out) noexcept;

// True when the event's "File Name" names a file inside |deviceFolder| (as DeviceFolderPath gives
// it), at any depth: the name starts with that path, ignoring case, then a separator and more
// (pure). So a file in a folder of the same name elsewhere (another user's "\app\<name>\"), or
// in a folder whose name only starts the same, does not count. An empty |deviceFolder| matches
// nothing.
bool FileNameIsInFolder(std::wstring_view fileName, std::wstring_view deviceFolder) noexcept;

// Whether one event read from the log is a block of the build in |deviceFolder| (pure):
// |rendered| is how rendering its "File Name" went, and |fileName| the name it gave. An event
// that could not be rendered never counts, whatever |fileName| holds.
bool EventCountsAsBlock(HRESULT rendered, std::wstring_view fileName,
                        std::wstring_view deviceFolder) noexcept;

// How reading the log went, from how many of its events were |rendered| and how many |failed| to
// render, |lastFailure| the error of the last that failed (pure): S_OK, also when a few events
// were passed over; but when there were events and none of them could be rendered, that is no
// answer rather than "no blocks": |lastFailure|, or HRESULT_FROM_WIN32(ERROR_INVALID_DATA) when
// it holds no error.
HRESULT RenderOutcome(int rendered, int failed, HRESULT lastFailure) noexcept;

// The event-log query for event 3077 of the last |days| days (pure; |days| between 1 and 365).
// May throw std::bad_alloc.
std::wstring CiBlockQuery(int days);

// Counts the events 3077 ("Code Integrity determined that a process attempted to load a file
// that did not meet the policy") of the last |days| days in the channel
// Microsoft-Windows-CodeIntegrity/Operational that EventCountsAsBlock for the build folder
// |buildFolder| (the registered DLL's folder, through DeviceFolderPath). |days| outside 1-365
// gives unreadable with E_INVALIDARG, and a folder DeviceFolderPath refuses or cannot find gives
// unreadable with its error, before the log is opened. Read-only; delay-loads wevtapi.dll.
BlockCount CountCiBlocks(const std::filesystem::path& buildFolder, int days) noexcept;

// ---- The verdict --------------------------------------------------------------------------

// True for the HRESULT of a refused image load (pure), exactly the codes 1.x recognized:
// HRESULT_FROM_WIN32 of 577 (ERROR_INVALID_IMAGE_HASH), 4551 (an Application Control policy has
// blocked this file), 4556-4559 and 4580-4583 (its other variants), which is how a Smart App
// Control block of PdfThumbnails.dll reaches the Explorer test.
bool IsLoadBlockedHresult(HRESULT hr) noexcept;

// Whether a check counts blocks at all (pure): Smart App Control is on, a class is registered and
// the DLL it names is there (|dllRegistered|; otherwise there is no build folder to look for, and
// a count would only fail to find it) and the Explorer test did not pass (a passing test settles
// the question; std::nullopt = it did not run).
bool ShouldCountBlocks(SacState state, bool dllRegistered, std::optional<HRESULT> probe) noexcept;

// Whether to wait kSacRecountDelayMs and count again (pure), as 1.x did: the first count found no
// blocks (none, or the log could not be read), the Explorer test ran and failed, and its error is
// not a load-blocked one (which settles the question already).
bool ShouldRecountBlocks(std::optional<HRESULT> probe, const BlockCount& first) noexcept;

enum class SacVerdictKind {
  kNone,       // off, or not on this Windows: nothing to say
  kEval,       // evaluation mode: it may switch on and then block
  kUnknown,    // a state value nobody documented: "unknown (<value>)"
  kOk,         // on, and the Explorer test passed
  kBlocked,    // on and blocking: block events, or a load-blocked error (breaks thumbnails)
  kOn,         // on; no blocks logged (or none counted: no class registered)
  kOnUnknown,  // on; the Code Integrity log could not be read
};

struct SacVerdict {
  SacVerdictKind kind = SacVerdictKind::kNone;
  int blocks = 0;  // kBlocked by events: how many
  // kBlocked by the Explorer test's error (no events counted): that error.
  std::optional<HRESULT> evidence;
};

// The verdict (pure): |probe| is the Explorer test's HRESULT (std::nullopt: it did not run),
// |blocks| the count (std::nullopt: not counted). For kOn:
//   probe S_OK                                   -> kOk (blocks are not looked at)
//   blocks readable and > 0                      -> kBlocked, blocks = the count
//   probe IsLoadBlockedHresult                   -> kBlocked, evidence = probe
//   blocks not counted, or readable and 0        -> kOn
//   blocks unreadable                            -> kOnUnknown
// kEvaluation -> kEval, kOff and kAbsent -> kNone, kUnknown -> kUnknown, whatever the rest.
SacVerdict DecideSac(SacState state, std::optional<HRESULT> probe,
                     const std::optional<BlockCount>& blocks) noexcept;

}  // namespace pdft
