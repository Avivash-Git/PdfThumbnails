// Running test code at Medium integrity.
//
// The handler refuses to render above Medium (as in 1.1), and CI jobs run as an administrator
// at High integrity. The handler tests therefore relaunch themselves at Medium with a copy of
// their own token whose integrity label is lowered (DuplicateTokenEx + SetTokenInformation +
// CreateProcessAsUserW: no privilege needed, the shipped DLL gets no bypass).
//
// pdft-run-medium runs any command that way, by default with a *filtered* token (Administrators
// deny-only, no privileges), so a child that writes machine state fails like a standard user's
// process would, also where UAC is on but the job token is a full administrator token.

#pragma once

#include <windows.h>

#include <string>

namespace pdft::test {

// Set (to "1") in the environment of a relaunched test process.
inline constexpr wchar_t kRelaunchedEnv[] = L"PDFT_TEST_RELAUNCHED";

// TokenIntegrityRid() when the label cannot be read; counts as "above Medium".
inline constexpr DWORD kUnknownIntegrity = MAXDWORD;

// The integrity RID (SECURITY_MANDATORY_*_RID) of |token| (opened with TOKEN_QUERY).
DWORD TokenIntegrityRid(HANDLE token);
// The integrity RID of this process.
DWORD CurrentIntegrityRid();
// True when |token| (opened with TOKEN_QUERY | TOKEN_DUPLICATE) has the Administrators group
// enabled (not deny-only). False when it cannot tell.
bool TokenHasEnabledAdministrators(HANDLE token);
// Prints this process's integrity level, whether Administrators is enabled, the elevation type,
// and the token's owner and default DACL as SDDL; returns 0 when it runs at Medium without
// Administrators enabled, else 3.
int ReportOwnToken();

// True for the errors Windows gives when Application Control (WDAC, Smart App Control) refuses
// to start a program: locally built, unsigned test programs can be blocked on a PC with Smart
// App Control on, and the tests then skip instead of failing.
bool IsApplicationControlBlock(HRESULT hr);

enum class MediumToken {
  kLabelOnly,  // the same token with the Medium label (D11; what the handler tests use)
  kFiltered,   // also Administrators deny-only and every privilege but SeChangeNotify removed
};

// A primary token like |source| at Medium integrity. |source| needs TOKEN_QUERY,
// TOKEN_DUPLICATE, TOKEN_ADJUST_DEFAULT and TOKEN_ASSIGN_PRIMARY (a filtered token keeps the
// source handle's access). The label is only ever lowered, which needs no privilege: a
// |source| below Medium gives E_INVALIDARG. The new token's owner is its user, and its default
// DACL grants its user and SYSTEM full access and its logon session read/execute, as a standard
// user's token does (an elevated source's default DACL grants only Administrators and SYSTEM).
HRESULT CreateMediumToken(HANDLE source, MediumToken kind, HANDLE* token);
// The same for this process's own token.
HRESULT CreateMediumTokenForSelf(MediumToken kind, HANDLE* token);

// Starts |commandLine| (program first, quoted as needed; |application| is the program's full
// path, or empty to take it from the command line) with |token| on the caller's desktop, with
// the caller's environment and standard handles, waits and returns the exit code.
HRESULT RunWithToken(HANDLE token, const std::wstring& application, const std::wstring& commandLine,
                     DWORD* exitCode);

// Quotes one argument for a command line (the rules CommandLineToArgvW and the CRT parse).
std::wstring QuoteArgument(const std::wstring& argument);

enum class RelaunchDecision {
  kRunHere,   // at or below Medium: nothing to do
  kRelaunch,  // above Medium: run again at Medium
  kRefuse,    // above Medium although already relaunched: stop (no relaunch loop)
};
RelaunchDecision DecideRelaunch(DWORD integrityRid, bool alreadyRelaunched);

// For test mains (call first). Above Medium, runs this program again with the same command
// line at Medium (label-only token, kRelaunchedEnv=1), sets |exitCode| to its exit code and
// returns true: the caller returns that code. Otherwise returns false and the caller runs its
// tests here; a relaunched child prints "relaunched at Medium integrity" first.
bool RelaunchAtMediumIfElevated(int* exitCode);

}  // namespace pdft::test
