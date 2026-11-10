// Who and where this process is: its user, whether it is elevated, the architectures, and
// whether it runs inside an app container whose registry virtualization would swallow its
// writes.
//
// The container case is real on the developer's PC: a terminal inside an MSIX-packaged app (and
// every program started from it) sees a private copy of HKCU. Writes land in that copy, which
// Explorer and the thumbnail handler never read; worse, the command would report success. So
// every command that writes first proves that its writes reach the real hive (the canary), and
// is refused otherwise (exit code 8).
//
// Commands that only report (status, diagnostics) write nothing and run the read-only check
// instead: they compare what this process reads with the real hive. Views that agree show only
// that reads reach the real hive. MSIX registry virtualization is copy-on-write, so a container
// whose reads agree can still swallow writes; the two checks therefore have separate results.
// Whether the report commands should also run the canary when a packaged ancestor is found, to
// report that writes would be swallowed, is for the check engine to decide: the canary's
// volatile probe key sits next to the product key and never recreates it, so running it would
// not change what a report looks at.
//
// Elevated steps that write for all users judge their own context the same way. Only where an
// app package is above them, or the walk up their parents fails, they write a volatile test value
// under HKEY_LOCAL_MACHINE\Software\Classes and another in their own user's hive and read both
// back through WMI; they go on only when both come back. A package alone proves nothing about
// where writes land.

#pragma once

#include <windows.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "exit_code.h"

namespace pdft {

// The canary's verdict: whether this process's registry writes are swallowed.
enum class Virtualization { kNo, kYes, kUnknown };

// Commands refuse to write unless virtualization is ruled out: "unknown" counts as "yes".
constexpr bool RefusesWrites(Virtualization virtualization) noexcept {
  return virtualization != Virtualization::kNo;
}

// The read-only check's answer: whether this process reads the real hive.
enum class RegistryViews {
  kNotChecked,  // the read-only check did not run
  kAgree,       // what could be compared is the same in both views
  kDiffer,      // a value differs: this process's reads are redirected
  kUnknown,     // a value could not be read, or inside a package there was nothing to compare
};

// The registry check GetProcessContext runs, and the ProcessContext field it fills.
enum class ContextProbe {
  kNone,      // none
  kReadOnly,  // views: compares values that may exist (status, diagnostics; writes nothing)
  kWrite,     // virtualization: the canary (commands that write) writes, reads back and
              // deletes a probe key
};

struct ProcessContext {
  std::wstring userSid;      // "S-1-5-21-...-1001"
  std::wstring userAccount;  // "DOMAIN\user"; the SID if the account cannot be looked up
  // The Administrators group is enabled in the token, or the integrity level is above Medium.
  // Either way the process must not do per-user work meant for the signed-in user.
  bool elevated = false;
  DWORD integrityRid = 0;  // SECURITY_MANDATORY_*_RID
  // The token's user is not the user whose shell (Explorer) owns the desktop: "Run as
  // different user". False when there is no shell window.
  bool differentFromShellUser = false;
  // The package full name of the nearest process, this one first, then its parents, that runs
  // in a package other than this app's own.
  std::optional<std::wstring> packagedAncestor;
  ContextProbe probe = ContextProbe::kNone;  // which registry check ran
  // The canary's verdict, filled only for ContextProbe::kWrite. For the other probes it stays
  // kNo without having been checked, so only a command that ran the canary may rely on it.
  Virtualization virtualization = Virtualization::kNo;
  // The read-only check's answer, filled only for ContextProbe::kReadOnly. kAgree says only
  // that this process reads the real hive, not that its writes would reach it: MSIX registry
  // virtualization is copy-on-write, and a container whose reads agree can still swallow writes
  // (seen in practice: the canary says yes while the views agree).
  RegistryViews views = RegistryViews::kNotChecked;
  // Why the registry check that ran could not decide (virtualization or views kUnknown): its
  // first failure. S_OK when it decided, or when there was simply nothing to compare.
  HRESULT probeError = S_OK;
  Arch processArch = Arch::kOther;
  Arch osArch = Arch::kOther;
};

// Fills |context|, running the registry check |probe|. Fails only when this process's own token
// cannot be read; a registry check that cannot decide gives kUnknown.
[[nodiscard]] HRESULT GetProcessContext(ContextProbe probe, ProcessContext* context) noexcept;

// ProcessContext::differentFromShellUser read again now. Where this process's own token or the
// shell's cannot be read it counts as another user's; without a shell window it is false.
bool ShellRunsAsAnotherUser() noexcept;

// Walks from this process up through its parents (at most 32, from a process snapshot) and
// returns the package full name of the first one that runs in a foreign package, or
// std::nullopt. Stops where a parent cannot be opened, or was created after its child (its
// process id was reused). A package that this executable itself was installed from is not
// foreign; a package this process merely inherited from the app that started it is.
[[nodiscard]] HRESULT FindPackagedAncestor(std::optional<std::wstring>* packageFullName) noexcept;

// The canary, for commands that write. Creates the volatile key
// HKCU\Software\<Product>.probe-<GUID> holding a random value, reads it back through WMI from
// HKEY_USERS\<|userSid|>, and deletes it again on every path, through the handle it made it with
// and never by its name: a key someone else put at that name meanwhile, a link included, stays as
// it is. Volatile keys never reach the disk, so even a crash leaves nothing after the next
// sign-in. |failure| (optional) receives the first failure behind a kUnknown verdict (creating or
// setting the key, connecting to WMI, reading it back), else S_OK; |probeKey| (optional) the
// key's name under HKCU.
[[nodiscard]] HRESULT ProbeRegistryVirtualization(const std::wstring& userSid,
                                                  Virtualization* result,
                                                  HRESULT* failure = nullptr,
                                                  std::wstring* probeKey = nullptr) noexcept;

// Like ProbeRegistryVirtualization, under HKEY_LOCAL_MACHINE: the volatile key
// Software\Classes\<Product>.probe-<GUID> (64-bit view), read back through WMI from
// HKEY_LOCAL_MACHINE, deleted the same way on every path. Without administrator rights the key
// cannot be made: kUnknown with that failure. |probeKey| (optional): the key's path under
// HKEY_LOCAL_MACHINE.
[[nodiscard]] HRESULT ProbeMachineRegistryVirtualization(Virtualization* result,
                                                         HRESULT* failure = nullptr,
                                                         std::wstring* probeKey = nullptr) noexcept;

// Where an elevated step that writes for all users runs, as far as app packages go.
struct MachineWriteCheck {
  bool packageAbove = false;  // FindPackagedAncestor found a foreign package
  HRESULT walk = S_OK;        // FindPackagedAncestor's failure, if it failed
  bool probed = false;        // the two canaries ran
  Virtualization machine = Virtualization::kUnknown;  // HKEY_LOCAL_MACHINE\Software\Classes
  Virtualization user = Virtualization::kUnknown;     // this process's user's own hive
  HRESULT probeError = S_OK;                          // the first failure behind a kUnknown
};

// Below an app package, or with the processes above unreadable: packageAbove or a failed walk
// (pure).
bool BelowPackage(const MachineWriteCheck& check) noexcept;

// Whether a step that writes for all users refuses (pure): below a package (BelowPackage) unless
// both canaries ran and showed that its writes reach the real registry; "unknown" counts as
// "virtualized".
bool RefusesMachineWrites(const MachineWriteCheck& check) noexcept;

// What |check| found (pure; never a name or a path): "no app package above"; else "app package
// above" or "processes above unreadable, walk-hr=<HRESULT>", then "; not checked" or "; HKLM <v>,
// HKCU <v>" (real, virtualized, unknown), then ", probe-hr=<HRESULT>" when probeError failed.
// May throw std::bad_alloc.
std::wstring MachineWriteCheckText(const MachineWriteCheck& check);

// The walk up this process's parents only (FindPackagedAncestor): resets *check first.
void ReadPackageAbove(MachineWriteCheck* check) noexcept;

// Both canaries: under HKEY_LOCAL_MACHINE (ProbeMachineRegistryVirtualization) and in this
// process's user's own hive (ProbeRegistryVirtualization), always both, so that the report shows
// both; sets probed. A canary that fails as a whole gives kUnknown and its failure.
void ProbeMachineWrites(MachineWriteCheck* check) noexcept;

// ReadPackageAbove, then ProbeMachineWrites when BelowPackage.
void CheckMachineWrites(MachineWriteCheck* check) noexcept;

// The read-only check, for commands that only report: compares this process's view of two
// values with WMI's view of the real hive: the .pdf thumbnail slot
// (HKCU\Software\Classes\.pdf\ShellEx\<IThumbnailProvider IID>, default value) and the zoom
// (HKCU\<product key>\Zoom). Either may be missing on both sides. |failure| (optional) receives
// the first read that failed behind a kUnknown answer, else S_OK.
[[nodiscard]] HRESULT CompareRegistryViews(const std::wstring& userSid, bool packagedAncestor,
                                           RegistryViews* result,
                                           HRESULT* failure = nullptr) noexcept;

// The canary's verdict (pure). The probe must have been written in-process and WMI must have
// answered, else kUnknown; WMI must see exactly the |written| value, else kYes.
Virtualization DecideCanary(bool probeWritten, bool wmiAnswered,
                            const std::optional<std::wstring>& seen,
                            std::wstring_view written) noexcept;

// One value as seen from inside this process and from outside (through WMI).
enum class ValueState { kMissing, kPresent, kUnreadable };
struct ValueView {
  ValueState state = ValueState::kMissing;
  std::wstring data;     // when kPresent (a DWORD in decimal)
  HRESULT error = S_OK;  // when kUnreadable: why
};
struct ValueViews {
  ValueView inProcess;
  ValueView outside;
};

// The read-only check's answer (pure): any value that differs between the views means kDiffer;
// otherwise an unreadable value means kUnknown; otherwise a value present and equal on both
// sides means kAgree. If every value is missing on both sides there is nothing to compare:
// kAgree without a packaged ancestor (nothing redirects reads there), kUnknown with one. Never
// kNotChecked.
RegistryViews DecideFromViews(std::span<const ValueViews> values, bool packagedAncestor) noexcept;

// IMAGE_FILE_MACHINE_* to Arch (pure).
Arch ArchFromMachine(USHORT machine) noexcept;

}  // namespace pdft
