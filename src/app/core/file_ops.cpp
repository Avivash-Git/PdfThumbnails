#include "file_ops.h"

#include <aclapi.h>
#include <ntsecapi.h>
#include <objbase.h>
#include <sddl.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "elevation.h"
#include "failure_trace.h"
#include "legacy.h"
#include "paths.h"
#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

// Files are copied in pieces of this size.
constexpr DWORD kCopyChunkBytes = 256 * 1024;

// Folders nested deeper than this are not ours (an install is at most a few levels deep).
constexpr int kMaxRemoveDepth = 16;

// How long RemoveTreeOrTrash waits between attempts on files in use.
constexpr DWORD kRetryStepMs = 250;

// More RunOnce values than this for one product are not ours to add to.
constexpr size_t kMaxRunOnceValues = 64;

HRESULT LastErrorOr(HRESULT fallback) noexcept {
  const DWORD error = GetLastError();
  return error == ERROR_SUCCESS ? fallback : HRESULT_FROM_WIN32(error);
}

bool IsMissingError(DWORD error) noexcept {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// The errors Windows gives for a file that another process has open or mapped.
bool IsFileInUseError(HRESULT hr) noexcept {
  return hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
         hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
         hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
         hr == HRESULT_FROM_WIN32(ERROR_USER_MAPPED_FILE);
}

void NoteFailure(HRESULT hr, HRESULT* first) noexcept {
  if (FAILED(hr) && SUCCEEDED(*first)) *first = hr;
}

// |path| without "." and "..", and without a trailing separator unless it is a root.
std::filesystem::path NormalPath(const std::filesystem::path& path) {
  std::filesystem::path normal = path.lexically_normal();
  if (normal.has_relative_path() && normal.filename().empty()) normal = normal.parent_path();
  return normal;
}

// 32 hexadecimal digits from a new GUID: a name nobody can guess beforehand.
HRESULT NewHexName(std::wstring* name) noexcept try {
  GUID guid = {};
  RETURN_IF_FAILED(CoCreateGuid(&guid));
  wchar_t text[33] = {};
  swprintf_s(text, L"%08lx%04hx%04hx%02x%02x%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2,
             guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4],
             guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  *name = text;
  return S_OK;
}
CATCH_RETURN();

// The open handle is a regular file on disk: not a link (opened as itself), not a folder, not a
// pipe or a device.
HRESULT CheckRegularFile(HANDLE file) noexcept {
  if (GetFileType(file) != FILE_TYPE_DISK) return HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE);
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  RETURN_IF_WIN32_BOOL_FALSE(
      GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)));
  if (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
    return HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE);
  }
  return S_OK;
}

// Deletes the file behind |file| when the handle closes.
void DeleteOnClose(HANDLE file) noexcept {
  FILE_DISPOSITION_INFO disposition = {};
  disposition.DeleteFile = TRUE;
  SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
}

// Renames the open file |file| to the absolute path |to|, replacing a file of that name. Windows
// resolves the name like any other path: a bare file name would land in the current folder.
HRESULT RenameByHandle(HANDLE file, const std::filesystem::path& to) noexcept try {
  RETURN_HR_IF(E_INVALIDARG, !to.is_absolute());
  const std::wstring& name = to.native();
  const size_t nameBytes = name.size() * sizeof(wchar_t);
  std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO) + nameBytes, 0);
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(nameBytes);
  std::memcpy(info->FileName, name.data(), nameBytes);
  const DWORD size = static_cast<DWORD>(buffer.size());
  if (SetFileInformationByHandle(file, FileRenameInfoEx, info, size)) return S_OK;
  const DWORD error = GetLastError();
  // File systems without the newer form (FAT, older NTFS drivers) take the classic one.
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
      error != ERROR_INVALID_FUNCTION) {
    return HRESULT_FROM_WIN32(error);
  }
  info->Flags = 0;
  info->ReplaceIfExists = TRUE;
  if (!SetFileInformationByHandle(file, FileRenameInfo, info, size)) return LastErrorOr(E_FAIL);
  return S_OK;
}
CATCH_RETURN();

// Reads |file| from where it stands to its end, hashing and, with |to|, writing every byte.
HRESULT Pump(HANDLE file, HANDLE to, std::vector<BYTE>* buffer, Sha256Digest* digest) noexcept {
  Sha256Stream hash;
  RETURN_IF_FAILED(hash.Start());
  for (;;) {
    DWORD read = 0;
    RETURN_IF_WIN32_BOOL_FALSE(
        ReadFile(file, buffer->data(), static_cast<DWORD>(buffer->size()), &read, nullptr));
    if (read == 0) break;
    RETURN_IF_FAILED(hash.Add(buffer->data(), read));
    if (to) {
      DWORD written = 0;
      RETURN_IF_WIN32_BOOL_FALSE(WriteFile(to, buffer->data(), read, &written, nullptr));
      RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), written != read);
    }
  }
  return hash.Finish(digest);
}

// Deletes a file, read-only or not.
HRESULT DeletePlainFile(const std::filesystem::path& file, DWORD attributes) noexcept {
  if (attributes & FILE_ATTRIBUTE_READONLY) {
    SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
  }
  if (DeleteFileW(file.c_str())) return S_OK;
  return LastErrorOr(E_FAIL);
}

// Deletes the file open as |file| (with DELETE, FILE_READ_ATTRIBUTES and FILE_WRITE_ATTRIBUTES),
// read-only or not, with POSIX semantics where the file system has them. S_OK with *result
// kDeleted, or kInUse when Windows refuses it as in use; a failure for other errors.
HRESULT DeleteByHandle(HANDLE file, ContentDelete* result) noexcept {
  FILE_DISPOSITION_INFO_EX dispositionEx = {};
  dispositionEx.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
  if (!SetFileInformationByHandle(file, FileDispositionInfoEx, &dispositionEx,
                                  sizeof(dispositionEx))) {
    const DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
        error != ERROR_INVALID_FUNCTION) {
      if (IsFileInUseError(HRESULT_FROM_WIN32(error))) {
        *result = ContentDelete::kInUse;
        return S_OK;
      }
      return HRESULT_FROM_WIN32(error);
    }
    // File systems without the newer form: clear read-only, then the classic disposition.
    FILE_BASIC_INFO basic = {};
    if (GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
      basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
      if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
      SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic));
    }
    FILE_DISPOSITION_INFO disposition = {};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
      const HRESULT hr = LastErrorOr(E_FAIL);
      if (IsFileInUseError(hr)) {
        *result = ContentDelete::kInUse;
        return S_OK;
      }
      return hr;
    }
  }
  *result = ContentDelete::kDeleted;
  return S_OK;
}

// What one removal pass found: entries in use, and the first error of each kind.
struct RemoveState {
  std::vector<std::filesystem::path> held;  // in use
  HRESULT heldError = S_OK;                 // the first entry in use's error
  HRESULT otherError = S_OK;                // the first other failure
};

// One pass over |dir|: deletes what it can, without following links, and notes in |state| what
// it could not. Fails only when out of memory.
HRESULT RemovePass(const std::filesystem::path& dir, int depth, RemoveState* state) noexcept try {
  if (depth > kMaxRemoveDepth) {
    NoteFailure(HRESULT_FROM_WIN32(ERROR_DIRECTORY), &state->otherError);
    return S_OK;
  }
  const auto note = [state](const std::filesystem::path& path, HRESULT hr) {
    if (IsFileInUseError(hr)) {
      state->held.push_back(path);
      NoteFailure(hr, &state->heldError);
    } else {
      NoteFailure(hr, &state->otherError);
    }
  };
  WIN32_FIND_DATAW entry = {};
  const std::filesystem::path pattern = dir / L"*";
  wil::unique_hfind find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry,
                                          FindExSearchNameMatch, nullptr,
                                          FIND_FIRST_EX_LARGE_FETCH));
  if (!find) {
    const DWORD error = GetLastError();
    if (error == ERROR_PATH_NOT_FOUND) return S_OK;  // gone already
    if (error != ERROR_FILE_NOT_FOUND) {
      NoteFailure(HRESULT_FROM_WIN32(error), &state->otherError);
      return S_OK;
    }
  } else {
    do {
      const std::wstring_view name = entry.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::filesystem::path child = dir / entry.cFileName;
      const DWORD attributes = entry.dwFileAttributes;
      if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // The link itself goes; what it points to is never entered.
        const BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY)
                                 ? RemoveDirectoryW(child.c_str())
                                 : DeleteFileW(child.c_str());
        if (!removed) note(child, LastErrorOr(E_FAIL));
      } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        RETURN_IF_FAILED(RemovePass(child, depth + 1, state));
      } else {
        const HRESULT hr = DeletePlainFile(child, attributes);
        if (FAILED(hr)) note(child, hr);
      }
    } while (FindNextFileW(find.get(), &entry));
    find.reset();
  }
  if (!RemoveDirectoryW(dir.c_str())) {
    const HRESULT hr = LastErrorOr(E_FAIL);
    if (hr == HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY) && !state->held.empty()) return S_OK;
    // In use: a folder some program has open (as its current folder, say).
    if (!IsMissingError(HRESULT_CODE(hr))) note(dir, hr);
  }
  return S_OK;
}
CATCH_RETURN();

bool Exists(const std::filesystem::path& path) noexcept {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ||
         !IsMissingError(GetLastError());
}

bool IsPlainFile(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

// cmd.exe's full path, from the system folder Windows itself reports.
HRESULT CmdExePath(std::filesystem::path* cmd) noexcept try {
  wchar_t system[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
  RETURN_LAST_ERROR_IF(length == 0);
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length >= ARRAYSIZE(system));
  *cmd = std::filesystem::path(system) / L"cmd.exe";
  return S_OK;
}
CATCH_RETURN();

// The names of every value of |subKey| under |root|; none when the key does not exist.
HRESULT ListValueNames(HKEY root, std::wstring_view subKey,
                       std::vector<std::wstring>* names) noexcept try {
  names->clear();
  wil::unique_hkey key;
  const std::wstring subKeyText(subKey);
  const LSTATUS opened =
      RegOpenKeyExW(root, subKeyText.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.put());
  if (opened == ERROR_FILE_NOT_FOUND) return S_OK;
  RETURN_IF_WIN32_ERROR(opened);
  DWORD count = 0;
  DWORD longest = 0;
  RETURN_IF_WIN32_ERROR(RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, nullptr,
                                         nullptr, &count, &longest, nullptr, nullptr, nullptr));
  std::wstring name(static_cast<size_t>(longest) + 1, L'\0');
  for (DWORD index = 0;; ++index) {
    DWORD length = static_cast<DWORD>(name.size());
    const LSTATUS status =
        RegEnumValueW(key.get(), index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status == ERROR_MORE_DATA) {  // a value was added meanwhile: more room, same index
      name.resize(name.size() * 2);
      --index;
      continue;
    }
    RETURN_IF_WIN32_ERROR(status);
    names->emplace_back(name.data(), length);
  }
  return S_OK;
}
CATCH_RETURN();

// ---- who else could change a build folder ----

constexpr ACCESS_MASK kOwnerImplicitRights = READ_CONTROL | WRITE_DAC;

// Windows paths cannot be longer than this (UNICODE_STRING counts bytes in 16 bits).
constexpr size_t kMaxPathChars = 32768;

// A folder is listed through its handle in pieces of this size.
constexpr DWORD kListBufferBytes = 64 * 1024;

// The identifier authorities of the SIDs the check looks at by their shape.
constexpr BYTE kCreatorAuthority = 3;      // CREATOR OWNER, CREATOR GROUP, OWNER RIGHTS
constexpr BYTE kNtAuthority = 5;           // accounts, groups and services
constexpr BYTE kAppPackageAuthority = 15;  // app containers and their capabilities

// TrustedInstaller's service SID: S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464.
constexpr DWORD kTrustedInstaller[] = {
    SECURITY_SERVICE_ID_BASE_RID, 956008885u, 3418522649u, 1831038044u, 1853292631u, 2271478464u};

struct WellKnownSid {
  BYTE bytes[SECURITY_MAX_SID_SIZE] = {};
  PSID sid() { return bytes; }
};

bool Contains(std::span<const PSID> set, PSID sid) noexcept {
  if (!sid || !IsValidSid(sid)) return false;
  return std::any_of(set.begin(), set.end(), [sid](PSID member) { return EqualSid(member, sid); });
}

// |sid|'s identifier authority is the small one |authority| (S-1-<authority>-...).
bool HasAuthority(PSID sid, BYTE authority) noexcept {
  const SID_IDENTIFIER_AUTHORITY* value = GetSidIdentifierAuthority(sid);
  return value->Value[0] == 0 && value->Value[1] == 0 && value->Value[2] == 0 &&
         value->Value[3] == 0 && value->Value[4] == 0 && value->Value[5] == authority;
}

std::span<const DWORD> SubAuthorities(PSID sid) noexcept {
  const UCHAR count = *GetSidSubAuthorityCount(sid);
  if (count == 0) return {};
  return std::span<const DWORD>(GetSidSubAuthority(sid, 0), count);
}

bool IsOwnerRights(PSID sid) noexcept {
  const std::span<const DWORD> subs = SubAuthorities(sid);
  return HasAuthority(sid, kCreatorAuthority) && subs.size() == 1 &&
         subs[0] == SECURITY_CREATOR_OWNER_RIGHTS_RID;
}

// The domain part of the account SID |sid| (S-1-5-21-a-b-c-<RID>): its sub-authorities without
// the RID; empty for any other SID.
std::span<const DWORD> AccountDomainOf(PSID sid) noexcept {
  if (!sid || !IsValidSid(sid) || !HasAuthority(sid, kNtAuthority)) return {};
  const std::span<const DWORD> subs = SubAuthorities(sid);
  if (subs.size() != 5 || subs[0] != SECURITY_NT_NON_UNIQUE) return {};
  return subs.first(4);
}

// |sid| is the user the check is made for (|user|, may be null), or an administrator by
// definition (file_ops.h): the RIDs 500 and 512 count only in |user|'s own domain and in
// |domains| (this PC's).
bool IsTrusted(PSID sid, PSID user, std::span<const PSID> domains) noexcept {
  if (user && EqualSid(sid, user)) return true;
  if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid)) {
    return true;
  }
  if (!HasAuthority(sid, kNtAuthority)) return false;
  const std::span<const DWORD> subs = SubAuthorities(sid);
  if (std::ranges::equal(subs, kTrustedInstaller)) return true;
  // S-1-5-21-<three numbers of the PC or the domain>-<RID>.
  const std::span<const DWORD> domain = AccountDomainOf(sid);
  if (domain.empty()) return false;
  const DWORD rid = subs[4];
  if (rid != DOMAIN_USER_RID_ADMIN && rid != DOMAIN_GROUP_RID_ADMINS) return false;
  if (user && std::ranges::equal(domain, AccountDomainOf(user))) return true;
  return std::ranges::any_of(domains, [domain](PSID trusted) {
    return trusted && IsValidSid(trusted) && HasAuthority(trusted, kNtAuthority) &&
           std::ranges::equal(domain, SubAuthorities(trusted));
  });
}

// The domains whose built-in Administrator and Domain Admins administer this PC
// (FindBroadAccess): its own accounts' and, when it is joined to one, its domain's, as LSA
// keeps them (both are local facts, read without asking a domain controller).
HRESULT ThisPcsDomains(std::vector<std::vector<BYTE>>* sids) noexcept try {
  sids->clear();
  LSA_OBJECT_ATTRIBUTES attributes = {};
  LSA_HANDLE policy = nullptr;
  RETURN_IF_WIN32_ERROR(LsaNtStatusToWinError(
      LsaOpenPolicy(nullptr, &attributes, POLICY_VIEW_LOCAL_INFORMATION, &policy)));
  const auto close = wil::scope_exit([policy] { LsaClose(policy); });
  for (const POLICY_INFORMATION_CLASS kind :
       {PolicyAccountDomainInformation, PolicyDnsDomainInformation}) {
    void* buffer = nullptr;
    RETURN_IF_WIN32_ERROR(LsaNtStatusToWinError(LsaQueryInformationPolicy(policy, kind, &buffer)));
    const auto free = wil::scope_exit([buffer] { LsaFreeMemory(buffer); });
    // A PC in a workgroup has no domain SID.
    const PSID sid = kind == PolicyAccountDomainInformation
                         ? static_cast<POLICY_ACCOUNT_DOMAIN_INFO*>(buffer)->DomainSid
                         : static_cast<POLICY_DNS_DOMAIN_INFO*>(buffer)->Sid;
    if (sid && IsValidSid(sid)) {
      const BYTE* bytes = static_cast<const BYTE*>(sid);
      sids->emplace_back(bytes, bytes + GetLengthSid(sid));
    }
  }
  return S_OK;
}
CATCH_RETURN();

// "S-1-5-11". May throw std::bad_alloc.
std::wstring SidText(PSID sid) {
  wil::unique_hlocal_string text;
  if (!sid || !ConvertSidToStringSidW(sid, text.put())) return {};
  return text.get();
}

// The allow forms: plain, with a condition, for an object type, and both.
bool IsAllowEntry(BYTE type) noexcept {
  return type == ACCESS_ALLOWED_ACE_TYPE || type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
         type == ACCESS_ALLOWED_OBJECT_ACE_TYPE || type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
}

// The deny forms that may not apply: with a condition, for an object type, and both.
bool IsDenyEntryThatMayNotApply(BYTE type) noexcept {
  return type == ACCESS_DENIED_CALLBACK_ACE_TYPE || type == ACCESS_DENIED_OBJECT_ACE_TYPE ||
         type == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE;
}

// The SID of an allow or deny entry in any of its forms; nullptr for an entry of another kind
// and for one too short to hold a whole SID.
PSID AceSid(const ACE_HEADER* header) noexcept {
  const size_t size = header->AceSize;
  size_t at = 0;
  switch (header->AceType) {
    case ACCESS_ALLOWED_ACE_TYPE:
    case ACCESS_DENIED_ACE_TYPE:
    case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
    case ACCESS_DENIED_CALLBACK_ACE_TYPE:
      at = offsetof(ACCESS_ALLOWED_ACE, SidStart);
      break;
    case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
    case ACCESS_DENIED_OBJECT_ACE_TYPE:
    case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
    case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE: {
      // The object forms keep a GUID or two before the SID, as their flags say.
      at = offsetof(ACCESS_ALLOWED_OBJECT_ACE, ObjectType);
      if (size < at) return nullptr;
      const DWORD flags = reinterpret_cast<const ACCESS_ALLOWED_OBJECT_ACE*>(header)->Flags;
      if (flags & ACE_OBJECT_TYPE_PRESENT) at += sizeof(GUID);
      if (flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) at += sizeof(GUID);
      break;
    }
    default:
      return nullptr;
  }
  // A SID is 8 bytes and 4 per sub-authority; the count is its second byte.
  constexpr size_t kSidHeaderBytes = 8;
  if (size < at + kSidHeaderBytes) return nullptr;
  BYTE* sid = reinterpret_cast<BYTE*>(const_cast<ACE_HEADER*>(header)) + at;
  if (size < at + kSidHeaderBytes + sizeof(DWORD) * sid[1] || !IsValidSid(sid)) return nullptr;
  return sid;
}

// The rights a token holding exactly the groups |set| gets from |dacl| (owned by |owner|),
// evaluated the way Windows does: entries in order, each right decided by the first entry that
// grants or denies it. |unknown| receives the rights that came from an entry of a kind this
// check does not know (or cannot read), which count for everyone.
ACCESS_MASK RightsOf(PACL dacl, PSID owner, std::span<const PSID> set,
                     ACCESS_MASK* unknown) noexcept {
  GENERIC_MAPPING mapping = {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                             FILE_ALL_ACCESS};
  const bool ownerInSet = Contains(set, owner);
  ACCESS_MASK granted = 0;
  ACCESS_MASK denied = 0;
  bool ownerRights = false;
  *unknown = 0;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* ace = nullptr;
    if (!GetAce(dacl, index, &ace)) break;
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    // Every access entry type starts with the header and the mask; one too short for its mask
    // counts as the worst.
    ACCESS_MASK mask = FILE_ALL_ACCESS;
    if (static_cast<size_t>(header->AceSize) >= sizeof(ACE_HEADER) + sizeof(ACCESS_MASK)) {
      mask = static_cast<const ACCESS_ALLOWED_ACE*>(ace)->Mask;
      MapGenericMask(&mask, &mapping);
    }
    const PSID sid = AceSid(header);
    const bool forOwner = sid && IsOwnerRights(sid);
    if (forOwner) ownerRights = true;
    const bool applies = sid && (forOwner ? ownerInSet : Contains(set, sid));
    if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
      if (applies) denied |= mask & ~granted;
    } else if (IsDenyEntryThatMayNotApply(header->AceType)) {
      // As if its condition or object type did not apply: it denies nothing for sure.
    } else if (IsAllowEntry(header->AceType) && sid) {
      // As if its condition or object type applied.
      if (applies) granted |= mask & ~denied;
    } else {
      *unknown |= mask & ~denied & ~granted;
      granted |= mask & ~denied;
    }
  }
  // Without an OWNER RIGHTS entry, the owner may always read and change the permissions.
  if (ownerInSet && !ownerRights) granted |= kOwnerImplicitRights;
  return granted;
}

// How the result names the entry of a kind this check does not know. May throw std::bad_alloc.
std::wstring UnknownEntryName(PACL dacl) {
  std::wstring name = L"everyone, through an access entry of a kind this check does not know";
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* ace = nullptr;
    if (!GetAce(dacl, index, &ace)) break;
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    if (header->AceType != ACCESS_DENIED_ACE_TYPE && !IsDenyEntryThatMayNotApply(header->AceType) &&
        !IsAllowEntry(header->AceType)) {
      return name + L" (type " + std::to_wstring(header->AceType) + L")";
    }
  }
  return name;
}

// How a link found on the way is put: what it is, from its reparse tag (0: a folder on the way
// led elsewhere). May throw std::bad_alloc.
std::wstring LinkWords(DWORD tag) {
  switch (tag) {
    case 0:
      return L"is reached through a link (it is not where its path leads)";
    case IO_REPARSE_TAG_MOUNT_POINT:
      return L"is a junction or mount point";
    case IO_REPARSE_TAG_SYMLINK:
      return L"is a symbolic link";
    default:
      break;
  }
  wchar_t hex[16] = {};
  swprintf_s(hex, L"0x%08lX", tag);
  return std::wstring(L"is a link (reparse tag ") + hex + L")";
}

UnsafeAccess LinkAccess(const std::filesystem::path& object, DWORD tag) {
  UnsafeAccess unsafe;
  unsafe.object = object;
  unsafe.kind = UnsafeKind::kLink;
  unsafe.reparseTag = tag;
  unsafe.access.principal = L"whoever can change what it leads to (it " + LinkWords(tag) + L")";
  return unsafe;
}

// Gives |access| the name Windows has for its SID ("PC\name"), when it has only the SID so far.
// Only what was found is named: the lookup may ask a domain controller.
void NameAccount(BroadAccess* access) {
  if (access->sid.empty() || access->principal != access->sid) return;
  wil::unique_hlocal sid;
  if (!ConvertStringSidToSidW(access->sid.c_str(), sid.put())) return;
  // Account and domain names are at most 256 characters (UNLEN).
  wchar_t name[257] = {};
  wchar_t domain[257] = {};
  DWORD nameLength = ARRAYSIZE(name);
  DWORD domainLength = ARRAYSIZE(domain);
  SID_NAME_USE use = SidTypeUnknown;
  if (!LookupAccountSidW(nullptr, sid.get(), name, &nameLength, domain, &domainLength, &use)) {
    return;
  }
  access->principal = domainLength > 0 ? std::wstring(domain) + L"\\" + name : std::wstring(name);
  access->lookedUp = true;
}

// |path| as an absolute drive path ("X:\..."), without "." and ".." and without a \\?\ prefix;
// E_INVALIDARG for anything else (a share, a device, a volume named by its GUID).
HRESULT DriveFolderPath(const std::filesystem::path& path, std::filesystem::path* out) noexcept
    try {
  std::wstring text = path.native();
  if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
  const bool drive =
      text.size() >= 3 &&
      ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
      text[1] == L':' && (text[2] == L'\\' || text[2] == L'/');
  RETURN_HR_IF(E_INVALIDARG, !drive);
  *out = NormalPath(text);
  return S_OK;
}
CATCH_RETURN();

// A relative name of something in a build's folder: not empty, no root, no "." or "..", no
// stream (":").
bool IsPlainRelativeName(std::wstring_view name) {
  const std::filesystem::path path(name);
  if (name.empty() || path.has_root_name() || path.has_root_directory() ||
      name.find(L':') != std::wstring_view::npos) {
    return false;
  }
  for (const std::filesystem::path& part : path) {
    if (part.empty() || part == L"." || part == L"..") return false;
  }
  return true;
}

// The access a folder above the build's folder, or the folder itself, is held with: enough to
// read its owner and permissions, and (traverse) to keep anyone from renaming or deleting it. A
// handle with none of read, write, execute or delete access would take no part in sharing.
constexpr DWORD kHeldFolderAccess =
    FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;

// Opens |path| itself, never what a link there leads to, to be held. The folders on the way
// (|onTheWay|) let others go on reading and writing in them, as the whole PC uses them; what is
// copied (files, and the folders of a tree) lets others only read. Neither lets anyone rename or
// delete it while it is open. A pipe's server gets an identification token at most.
HRESULT OpenHeld(const std::filesystem::path& path, bool onTheWay,
                 wil::unique_hfile* handle) noexcept {
  const DWORD access = onTheWay ? kHeldFolderAccess : GENERIC_READ;
  const DWORD share = onTheWay ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ;
  handle->reset(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                            nullptr));
  if (!*handle) return LastErrorOr(E_FAIL);
  return S_OK;
}

// What an open handle is.
enum class HeldKind {
  kFolder,  // a folder, another kind of reparse point (cloud files) included
  kFile,    // a regular file on disk
  kLink,    // a link, junction or mount point (a name surrogate)
  kOther,   // a file with another kind of reparse point, a device
};

HRESULT Classify(HANDLE handle, HeldKind* kind, DWORD* tag) noexcept {
  FILE_ATTRIBUTE_TAG_INFO info = {};
  RETURN_IF_WIN32_BOOL_FALSE(
      GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)));
  *tag = (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? info.ReparseTag : 0;
  if (*tag != 0 && IsReparseTagNameSurrogate(*tag) != 0) {
    *kind = HeldKind::kLink;
  } else if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    *kind = HeldKind::kFolder;
  } else if (*tag != 0 || GetFileType(handle) != FILE_TYPE_DISK) {
    *kind = HeldKind::kOther;
  } else {
    *kind = HeldKind::kFile;
  }
  return S_OK;
}

// Where Windows resolves the open |handle| to, on the volume's device
// (\Device\HarddiskVolume3\Users\...), without a separator at its end.
HRESULT FinalNtPath(HANDLE handle, std::wstring* path) noexcept try {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    RETURN_LAST_ERROR_IF(length == 0);
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    // Too small: |length| is the size needed, terminating null included.
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length > kMaxPathChars);
    buffer.resize(length);
  }
  while (!buffer.empty() && buffer.back() == L'\\') buffer.pop_back();
  *path = std::move(buffer);
  return S_OK;
}
CATCH_RETURN();

// Whether the open |handle| lies directly in the folder whose final path is |parent|
// (FinalNtPath), so that nothing on the way led elsewhere since that folder was opened. |own|
// receives its own final path.
HRESULT LiesIn(HANDLE handle, const std::wstring& parent, std::wstring* own, bool* inside) noexcept
    try {
  *inside = false;
  RETURN_IF_FAILED(FinalNtPath(handle, own));
  const size_t slash = own->rfind(L'\\');
  *inside =
      slash != std::wstring::npos && IEquals(std::wstring_view(*own).substr(0, slash), parent);
  return S_OK;
}
CATCH_RETURN();

// One entry of a folder listed through its handle.
struct ListedEntry {
  std::wstring name;
  DWORD attributes = 0;
};

// The entries of the open folder |folder| (not "." and ".."), sorted by name ignoring case.
// Listed through the handle, so it is the folder that was opened and checked, whatever its path
// would lead to now.
HRESULT ListOpenFolder(HANDLE folder, std::vector<ListedEntry>* entries) noexcept try {
  entries->clear();
  std::vector<ULONGLONG> buffer(kListBufferBytes / sizeof(ULONGLONG));
  FILE_INFO_BY_HANDLE_CLASS kind = FileFullDirectoryRestartInfo;
  for (;;) {
    if (!GetFileInformationByHandleEx(folder, kind, buffer.data(), kListBufferBytes)) {
      const DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) break;
      return HRESULT_FROM_WIN32(error);
    }
    kind = FileFullDirectoryInfo;
    const BYTE* at = reinterpret_cast<const BYTE*>(buffer.data());
    for (;;) {
      const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(at);
      const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        entries->push_back(ListedEntry{std::wstring(name), entry->FileAttributes});
      }
      if (entry->NextEntryOffset == 0) break;
      at += entry->NextEntryOffset;
    }
  }
  std::sort(entries->begin(), entries->end(), [](const ListedEntry& a, const ListedEntry& b) {
    return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  return S_OK;
}
CATCH_RETURN();

// "2 levels up": a folder above the build's folder in a trace, never its name.
std::wstring LevelsUp(size_t up) {
  return std::to_wstring(up) + (up == 1 ? L" level up" : L" levels up");
}

// Something HoldBuildFolder opened, as it is judged.
struct HeldObject {
  std::filesystem::path path;  // what is reported when it is unsafe
  std::wstring detail;         // the trace detail: a name, or how many levels up; never a path
  ACCESS_MASK mask = 0;        // the rights that count on it
  HANDLE handle = nullptr;     // owned by a Gathered
};

// What HoldBuildFolder opened before it judges anything.
struct Gathered {
  std::vector<std::filesystem::path> files;    // relative to the build's folder
  std::vector<wil::unique_hfile> fileHandles;  // one per entry of |files|
  std::vector<wil::unique_hfile> folders;      // the folders above, the folder, the tree's
  std::vector<HeldObject> judged;              // the folder, then what is copied, in order
  std::vector<HeldObject> above;               // the folders above, the nearest first
};

// Holds the tree folder |relative| (in the build's folder |dir|, open as |handle|, whose final
// path is |handlePath|, |depth| folders deep) and what is in it: every regular file, and every
// folder while |depth| is below |maxDepth|. A link found goes to |unsafe|, and the walk stops.
HRESULT HoldTree(const std::filesystem::path& dir, const std::filesystem::path& relative,
                 HANDLE handle, const std::wstring& handlePath, int depth, int maxDepth,
                 Gathered* gathered, std::optional<UnsafeAccess>* unsafe) noexcept try {
  std::vector<ListedEntry> entries;
  RETURN_IF_FAILED(
      PDFT_TRACE_DETAIL(ListOpenFolder(handle, &entries), relative.filename().native()));
  for (const ListedEntry& entry : entries) {
    const bool listedAsFolder = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (listedAsFolder && depth >= maxDepth) continue;  // deeper than notices are copied from
    const std::filesystem::path childRelative = relative / entry.name;
    const std::filesystem::path child = dir / childRelative;
    wil::unique_hfile opened;
    RETURN_IF_FAILED(PDFT_TRACE_DETAIL(OpenHeld(child, false, std::addressof(opened)), entry.name));
    HeldKind kind = HeldKind::kOther;
    DWORD tag = 0;
    RETURN_IF_FAILED(PDFT_TRACE_DETAIL(Classify(opened.get(), &kind, &tag), entry.name));
    if (kind == HeldKind::kLink) {
      *unsafe = LinkAccess(child, tag);
      return S_OK;
    }
    // Another kind of reparse point (cloud files, deduplication) or a device: not copied.
    if (tag != 0 || kind == HeldKind::kOther) continue;
    if (kind == HeldKind::kFolder && depth >= maxDepth) continue;
    std::wstring childPath;
    bool inside = false;
    RETURN_IF_FAILED(
        PDFT_TRACE_DETAIL(LiesIn(opened.get(), handlePath, &childPath, &inside), entry.name));
    if (!inside) {
      *unsafe = LinkAccess(child, 0);
      return S_OK;
    }
    const HANDLE held = opened.get();
    if (kind == HeldKind::kFolder) {
      gathered->folders.push_back(std::move(opened));
      gathered->judged.push_back(HeldObject{child, entry.name, kBuildObjectChangeRights, held});
      RETURN_IF_FAILED(
          HoldTree(dir, childRelative, held, childPath, depth + 1, maxDepth, gathered, unsafe));
      if (*unsafe) return S_OK;
    } else {
      gathered->files.push_back(childRelative);
      gathered->fileHandles.push_back(std::move(opened));
      gathered->judged.push_back(
          HeldObject{child, entry.name, kBuildObjectChangeRights & ~FILE_DELETE_CHILD, held});
    }
  }
  return S_OK;
}
CATCH_RETURN();

// The first account or group that is not trusted and may change the open |object| (|mask|),
// for |user|, with this PC's |domains| (ThisPcsDomains).
HRESULT JudgeAccess(HANDLE object, ACCESS_MASK mask, PSID user, std::span<const PSID> domains,
                    std::optional<BroadAccess>* found) noexcept try {
  found->reset();
  wil::unique_hlocal_security_descriptor descriptor;
  RETURN_IF_WIN32_ERROR(GetSecurityInfo(object, SE_FILE_OBJECT,
                                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, nullptr, nullptr, descriptor.put()));
  *found = FindBroadAccess(descriptor.get(), mask, user, domains);
  return S_OK;
}
CATCH_RETURN();

}  // namespace

HRESULT CheckNoReparsePoints(const std::filesystem::path& path,
                             const std::filesystem::path& stopAt) noexcept try {
  if (!path.is_absolute() || (!stopAt.empty() && !stopAt.is_absolute())) {
    RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  }
  const std::filesystem::path target = NormalPath(path);
  const std::filesystem::path stop = stopAt.empty() ? target.root_path() : NormalPath(stopAt);
  std::vector<std::filesystem::path> chain;
  for (std::filesystem::path part = target;; part = part.parent_path()) {
    chain.push_back(part);
    if (IEquals(part.native(), stop.native())) break;
    // At the root without meeting |stopAt|: it is not above |path|.
    if (!part.has_relative_path()) RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  }
  // From the top down: below a missing folder nothing exists either. A failure's trace detail
  // is how deep the folder lies below the top, never its name.
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const std::wstring depth = L"depth " + std::to_wstring(it - chain.rbegin());
    const DWORD attributes = GetFileAttributesW(it->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = GetLastError();
      if (IsMissingError(error)) return S_OK;
      return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(error), depth);
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK), depth);
    }
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT CopyFileVerified(const std::filesystem::path& from, const std::filesystem::path& to,
                         Sha256Digest* sha) noexcept try {
  if (sha) sha->fill(0);
  if (!from.is_absolute() || !to.is_absolute() || !to.has_filename()) {
    RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  }
  // Read sharing only: nobody can change or delete the source while it is copied, and a
  // process that already has it open for writing makes this open fail. A path that names a pipe
  // gives its server an identification token at most.
  wil::unique_hfile source(CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN |
                                           SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                       nullptr));
  if (!source) return PDFT_TRACE_DETAIL(LastErrorOr(E_FAIL), to.filename().native());
  return CopyOpenFileVerified(source.get(), to, sha);
}
CATCH_RETURN();

HRESULT CopyOpenFileVerified(HANDLE from, const std::filesystem::path& to,
                             Sha256Digest* sha) noexcept try {
  if (sha) sha->fill(0);
  if (!to.is_absolute() || !to.has_filename()) RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  // A failure's trace detail: the file's name, never its path.
  const std::wstring name = to.filename().native();
  const std::filesystem::path folder = NormalPath(to.parent_path());
  const DWORD folderAttributes = GetFileAttributesW(folder.c_str());
  if (folderAttributes == INVALID_FILE_ATTRIBUTES) {
    return PDFT_TRACE_DETAIL(LastErrorOr(E_FAIL), name);
  }
  if (folderAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK), name);
  }
  if (!(folderAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    RETURN_HR(PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_DIRECTORY), name));
  }
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(CheckRegularFile(from), name));
  LARGE_INTEGER origin = {};
  if (!SetFilePointerEx(from, origin, nullptr, FILE_BEGIN)) {
    RETURN_HR(PDFT_TRACE_DETAIL(LastErrorOr(E_FAIL), name));
  }

  std::wstring unique;
  RETURN_IF_FAILED(PDFT_TRACE(NewHexName(&unique)));
  const std::filesystem::path temporary = folder / (name + L"." + unique + L".tmp");
  // A path that names a pipe gives its server an identification token at most.
  wil::unique_hfile target(CreateFileW(
      temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (!target) return PDFT_TRACE_DETAIL(LastErrorOr(E_FAIL), name);
  // From here on every failure deletes the temporary file again.
  auto discard = wil::scope_exit([&]() noexcept { DeleteOnClose(target.get()); });

  std::vector<BYTE> buffer(kCopyChunkBytes);
  Sha256Digest copied = {};
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(Pump(from, target.get(), &buffer, &copied), name));
  if (!FlushFileBuffers(target.get())) RETURN_HR(PDFT_TRACE_DETAIL(LastErrorOr(E_FAIL), name));
  LARGE_INTEGER start = {};
  if (!SetFilePointerEx(target.get(), start, nullptr, FILE_BEGIN)) {
    RETURN_HR(PDFT_TRACE_DETAIL(LastErrorOr(E_FAIL), name));
  }
  Sha256Digest written = {};
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(Pump(target.get(), nullptr, &buffer, &written), name));
  if (written != copied) {
    RETURN_HR(PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_DATA_CHECKSUM_ERROR), name));
  }

  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(RenameByHandle(target.get(), NormalPath(to)), name));
  discard.release();
  if (sha) *sha = copied;
  return S_OK;
}
CATCH_RETURN();

HRESULT RemoveTreeNoFollow(const std::filesystem::path& dir) noexcept try {
  RemoveState state;
  RETURN_IF_FAILED(RemovePass(NormalPath(dir), 0, &state));
  RETURN_IF_FAILED(state.otherError);
  return state.heldError;
}
CATCH_RETURN();

HRESULT MakeTrashFolder(const std::filesystem::path& trashParent,
                        std::filesystem::path* trash) noexcept try {
  trash->clear();
  RETURN_HR_IF(E_INVALIDARG, !trashParent.is_absolute());
  const std::filesystem::path parent = NormalPath(trashParent);
  const DWORD attributes = GetFileAttributesW(parent.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return LastErrorOr(E_FAIL);
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  }
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_DIRECTORY), !(attributes & FILE_ATTRIBUTE_DIRECTORY));
  std::wstring unique;
  RETURN_IF_FAILED(NewHexName(&unique));
  const std::filesystem::path folder = parent / (std::wstring(kProductName) + L".trash-" + unique);
  // Fails if the name exists in any form, so the folder is surely a new, real one.
  RETURN_IF_WIN32_BOOL_FALSE(CreateDirectoryW(folder.c_str(), nullptr));
  *trash = folder;
  return S_OK;
}
CATCH_RETURN();

bool IsTrashFolderName(std::wstring_view name) noexcept {
  constexpr std::wstring_view kTrash = L".trash-";
  const std::wstring_view product = kProductName;
  if (!IStartsWith(name, product)) return false;
  name.remove_prefix(product.size());
  if (!IStartsWith(name, kTrash)) return false;
  name.remove_prefix(kTrash.size());
  return name.size() == 32 && std::all_of(name.begin(), name.end(), [](wchar_t c) {
           return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
         });
}

HRESULT MoveIntoTrash(const std::filesystem::path& file, const std::filesystem::path& trash,
                      std::filesystem::path* moved) noexcept try {
  if (moved) moved->clear();
  RETURN_HR_IF(E_INVALIDARG, !file.is_absolute() || !trash.is_absolute());
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE), !IsPlainFile(file));
  std::wstring unique;
  RETURN_IF_FAILED(NewHexName(&unique));
  const std::filesystem::path to = NormalPath(trash) / (unique + file.extension().native());
  // No MOVEFILE_COPY_ALLOWED: a file in use can only be renamed, never copied and deleted.
  if (!MoveFileExW(file.c_str(), to.c_str(), 0)) return LastErrorOr(E_FAIL);
  if (moved) *moved = to;
  return S_OK;
}
CATCH_RETURN();

HRESULT ScheduleDeleteAtRestart(const std::filesystem::path& path) noexcept {
  RETURN_HR_IF(E_INVALIDARG, !path.is_absolute());
  if (!MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) return LastErrorOr(E_FAIL);
  return S_OK;
}

HRESULT RemoveTreeOrTrash(const std::filesystem::path& dir,
                          const std::filesystem::path& trashParent, DWORD retryMs,
                          RemoveOutcome* outcome) noexcept try {
  *outcome = RemoveOutcome{};
  RETURN_HR_IF(E_INVALIDARG,
               !dir.is_absolute() || (!trashParent.empty() && !trashParent.is_absolute()));
  const std::filesystem::path folder = NormalPath(dir);
  RETURN_HR_IF(E_INVALIDARG, !folder.has_relative_path());  // never a whole volume
  const DWORD attributes = GetFileAttributesW(folder.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (!IsMissingError(error)) return HRESULT_FROM_WIN32(error);
    outcome->removed = true;
    return S_OK;
  }
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  }
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_DIRECTORY), !(attributes & FILE_ATTRIBUTE_DIRECTORY));

  RemoveState state;
  const ULONGLONG deadline = GetTickCount64() + retryMs;
  for (;;) {
    state = RemoveState{};
    RETURN_IF_FAILED(RemovePass(folder, 0, &state));
    const ULONGLONG now = GetTickCount64();
    if (state.held.empty() || now >= deadline) break;
    Sleep(static_cast<DWORD>(std::min<ULONGLONG>(kRetryStepMs, deadline - now)));
  }
  HRESULT other = state.otherError;

  bool elevated = false;
  if (!state.held.empty() && !trashParent.empty() && SUCCEEDED(GetTokenElevation(&elevated)) &&
      elevated) {
    std::filesystem::path trash;
    const HRESULT made = MakeTrashFolder(trashParent, &trash);
    NoteFailure(made, &other);
    if (SUCCEEDED(made)) {
      std::vector<std::filesystem::path> trashed;
      for (const std::filesystem::path& file : state.held) {
        std::filesystem::path moved;
        if (IsPlainFile(file) && SUCCEEDED(MoveIntoTrash(file, trash, &moved))) {
          trashed.push_back(std::move(moved));
        }
      }
      // What is left of |dir| now holds nothing in use, except what could not be moved.
      state = RemoveState{};
      RETURN_IF_FAILED(RemovePass(folder, 0, &state));
      NoteFailure(state.otherError, &other);
      if (trashed.empty()) {
        RemoveDirectoryW(trash.c_str());
      } else {
        // Files first: the folder can go only once it is empty.
        for (const std::filesystem::path& file : trashed) {
          NoteFailure(ScheduleDeleteAtRestart(file), &other);
        }
        NoteFailure(ScheduleDeleteAtRestart(trash), &other);
        outcome->trash = trash;
        outcome->restartPending = true;
      }
    }
  }
  if (!state.held.empty()) outcome->restartPending = true;
  outcome->left = std::move(state.held);
  outcome->removed = !Exists(folder);
  return other;
}
CATCH_RETURN();

std::wstring RunOnceValueName(size_t index) {
  std::wstring name = std::wstring(kProductName) + L"Cleanup";
  if (index > 0) name += std::to_wstring(index + 1);
  return name;
}

bool IsRunOnceRemovalName(std::wstring_view name) noexcept {
  if (IEquals(name, kLegacyRunOnce)) return true;
  constexpr std::wstring_view kCleanup = L"Cleanup";
  const std::wstring_view product = kProductName;
  if (!IStartsWith(name, product)) return false;
  name.remove_prefix(product.size());
  if (!IStartsWith(name, kCleanup)) return false;
  name.remove_prefix(kCleanup.size());
  return std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

HRESULT RunOnceRemovalCommand(const std::filesystem::path& cmdExe, const std::filesystem::path& dir,
                              std::wstring* command) {
  command->clear();
  std::wstring text = dir.native();
  while (text.size() > 3 && text.back() == L'\\') text.pop_back();
  const bool driveLetter = text.size() >= 4 && ((text[0] >= L'A' && text[0] <= L'Z') ||
                                                (text[0] >= L'a' && text[0] <= L'z'));
  if (!driveLetter || text[1] != L':' || text[2] != L'\\' || text.find(L'/') != text.npos ||
      text.find(L':', 2) != text.npos) {
    return E_INVALIDARG;
  }
  for (const std::filesystem::path& part : std::filesystem::path(text).relative_path()) {
    if (part.native().empty() || part == L"." || part == L"..") return E_INVALIDARG;
  }
  // cmd.exe expands %NAME% inside quotes too, and !NAME! where delayed expansion is on.
  for (const wchar_t c : text) {
    if (c < L' ' || c == L'"' || c == L'%' || c == L'!')
      return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
  }
  RETURN_HR_IF(E_INVALIDARG,
               !cmdExe.is_absolute() || cmdExe.native().find(L'"') != std::wstring::npos);
  std::wstring line = L"\"" + cmdExe.native() + L"\" /d /c rd /s /q \"" + text + L"\"";
  if (line.size() >= kRunOnceMaxChars) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  *command = std::move(line);
  return S_OK;
}

HRESULT ScheduleRunOnceRemoval(const RegistryRoots& roots,
                               std::span<const std::filesystem::path> dirs,
                               std::vector<std::filesystem::path>* notScheduled) noexcept try {
  if (notScheduled) notScheduled->clear();
  std::filesystem::path cmd;
  RETURN_IF_FAILED(CmdExePath(&cmd));
  std::vector<std::wstring> names;
  RETURN_IF_FAILED(ListValueNames(roots.user, kRunOnceKey, &names));
  std::vector<std::wstring> commands;  // what the family's values hold now
  for (const std::wstring& name : names) {
    if (!IsRunOnceRemovalName(name)) continue;
    std::optional<std::wstring> data;
    if (SUCCEEDED(ReadRegistryString(roots.user, kRunOnceKey, name, &data)) && data) {
      commands.push_back(std::move(*data));
    }
  }
  bool skipped = false;
  for (const std::filesystem::path& dir : dirs) {
    std::wstring command;
    if (FAILED(RunOnceRemovalCommand(cmd, dir, &command))) {
      if (notScheduled) notScheduled->push_back(dir);
      skipped = true;
      continue;
    }
    if (std::any_of(commands.begin(), commands.end(),
                    [&](const std::wstring& existing) { return IEquals(existing, command); })) {
      continue;
    }
    size_t index = 0;
    for (; index < kMaxRunOnceValues; ++index) {
      const std::wstring name = RunOnceValueName(index);
      if (std::none_of(names.begin(), names.end(),
                       [&](const std::wstring& taken) { return IEquals(taken, name); })) {
        break;
      }
    }
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES), index == kMaxRunOnceValues);
    const std::wstring name = RunOnceValueName(index);
    RETURN_IF_FAILED(WriteRegistryString(roots.user, kRunOnceKey, name, command));
    names.push_back(name);
    commands.push_back(std::move(command));
  }
  return skipped ? S_FALSE : S_OK;
}
CATCH_RETURN();

HRESULT DeleteRunOnceRemovals(const RegistryRoots& roots, size_t* deleted) noexcept try {
  if (deleted) *deleted = 0;
  std::vector<std::wstring> names;
  RETURN_IF_FAILED(ListValueNames(roots.user, kRunOnceKey, &names));
  for (const std::wstring& name : names) {
    if (!IsRunOnceRemovalName(name)) continue;
    bool gone = false;
    RETURN_IF_FAILED(DeleteRegistryValue(roots.user, kRunOnceKey, name, &gone));
    if (gone && deleted) ++*deleted;
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT FindRunOnceRemovals(const RegistryRoots& roots, std::vector<std::wstring>* names) noexcept
    try {
  names->clear();
  std::vector<std::wstring> all;
  RETURN_IF_FAILED(ListValueNames(roots.user, kRunOnceKey, &all));
  for (std::wstring& name : all) {
    if (IsRunOnceRemovalName(name)) names->push_back(std::move(name));
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT DeleteFileIfContent(const std::filesystem::path& file,
                            std::span<const Sha256Digest> allowed, ContentDelete* result) noexcept
    try {
  *result = ContentDelete::kMissing;
  RETURN_HR_IF(E_INVALIDARG, !file.is_absolute());
  // Others may go on reading it; nobody may write, rename or delete it while it is judged. A
  // path that names a pipe gives its server an identification token at most.
  wil::unique_hfile handle(CreateFileW(
      file.c_str(), DELETE | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN |
          SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
      nullptr));
  if (!handle) {
    const DWORD error = GetLastError();
    if (IsMissingError(error)) return S_OK;
    if (IsFileInUseError(HRESULT_FROM_WIN32(error))) {
      *result = ContentDelete::kInUse;
      return S_OK;
    }
    return HRESULT_FROM_WIN32(error);
  }
  if (FAILED(CheckRegularFile(handle.get()))) {
    *result = ContentDelete::kNotAFile;
    return S_OK;
  }
  if (!allowed.empty()) {
    std::vector<BYTE> buffer(kCopyChunkBytes);
    Sha256Digest digest = {};
    RETURN_IF_FAILED(Pump(handle.get(), nullptr, &buffer, &digest));
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const Sha256Digest& candidate) { return candidate == digest; })) {
      *result = ContentDelete::kMismatch;
      return S_OK;
    }
  }
  return DeleteByHandle(handle.get(), result);
}
CATCH_RETURN();

HRESULT DeleteRegularFile(const std::filesystem::path& file, ContentDelete* result) noexcept {
  *result = ContentDelete::kMissing;
  RETURN_HR_IF(E_INVALIDARG, !file.is_absolute());
  // Others may go on reading, writing and deleting it, as with DeleteFileW: only a program that
  // does not allow deleting it keeps it. A path that names a pipe gives its server an
  // identification token at most.
  wil::unique_hfile handle(
      CreateFileW(file.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS |
                      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                  nullptr));
  if (!handle) {
    const DWORD error = GetLastError();
    if (IsMissingError(error)) return S_OK;
    if (IsFileInUseError(HRESULT_FROM_WIN32(error))) {
      *result = ContentDelete::kInUse;
      return S_OK;
    }
    return HRESULT_FROM_WIN32(error);
  }
  if (FAILED(CheckRegularFile(handle.get()))) {
    *result = ContentDelete::kNotAFile;
    return S_OK;
  }
  return DeleteByHandle(handle.get(), result);
}

std::optional<BroadAccess> FindBroadAccess(PSECURITY_DESCRIPTOR descriptor, ACCESS_MASK mask,
                                           PSID user, std::span<const PSID> domains) {
  constexpr wchar_t kEveryoneSid[] = L"S-1-1-0";
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  PSID owner = nullptr;
  if (!descriptor || !GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) ||
      !GetSecurityDescriptorOwner(descriptor, &owner, &defaulted)) {
    return BroadAccess{L"Everyone", mask, kEveryoneSid};  // unreadable: assume the worst
  }

  std::array<WellKnownSid, 4> sids;
  constexpr WELL_KNOWN_SID_TYPE kTypes[] = {WinWorldSid, WinAuthenticatedUserSid,
                                            WinBuiltinUsersSid, WinInteractiveSid};
  constexpr const wchar_t* kNames[] = {L"Everyone", L"Authenticated Users", L"Users",
                                       L"INTERACTIVE"};
  for (size_t i = 0; i < sids.size(); ++i) {
    DWORD size = SECURITY_MAX_SID_SIZE;
    if (!CreateWellKnownSid(kTypes[i], nullptr, sids[i].sid(), &size)) {
      return BroadAccess{L"Everyone", mask, kEveryoneSid};
    }
  }
  const PSID everyone = sids[0].sid();
  const std::array<PSID, 4> broad = {everyone, sids[1].sid(), sids[2].sid(), sids[3].sid()};
  // A broad group by its name, any other SID by its text.
  const auto nameOf = [&](PSID sid) -> std::wstring {
    for (size_t i = 0; i < broad.size(); ++i) {
      if (EqualSid(sid, broad[i])) return kNames[i];
    }
    return SidText(sid);
  };

  // An owner may always change the permissions, whatever the DACL says.
  if (!owner || !IsValidSid(owner)) {
    return BroadAccess{L"nobody (it has no owner)", mask & kOwnerImplicitRights, L"", true};
  }
  if (!IsTrusted(owner, user, domains)) {
    return BroadAccess{nameOf(owner), mask & kOwnerImplicitRights, SidText(owner), true};
  }
  // A null DACL grants everything.
  if (!present || !dacl) return BroadAccess{L"Everyone", mask, kEveryoneSid};

  struct Shape {
    std::wstring name;
    std::wstring sid;
    std::array<PSID, 4> set;
    size_t count;
  };
  std::vector<Shape> shapes = {
      {kNames[0], SidText(everyone), {everyone}, 1},
      {kNames[1], SidText(broad[1]), {everyone, broad[1]}, 2},
      {kNames[2], SidText(broad[2]), {everyone, broad[2]}, 2},
      {kNames[3], SidText(broad[3]), {everyone, broad[3]}, 2},
      {L"Authenticated Users, Users and INTERACTIVE together", L"", broad, 4},
  };
  // Every other SID an allow entry names, on its own with Everyone. Trusted ones never count;
  // CREATOR OWNER and CREATOR GROUP match nobody, OWNER RIGHTS is the (trusted) owner; app
  // containers and capabilities get only what their user and groups get as well.
  std::vector<PSID> others;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* ace = nullptr;
    if (!GetAce(dacl, index, &ace)) break;
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    if ((header->AceFlags & INHERIT_ONLY_ACE) || !IsAllowEntry(header->AceType)) continue;
    const PSID sid = AceSid(header);
    if (!sid || IsTrusted(sid, user, domains) || Contains(broad, sid) ||
        HasAuthority(sid, kCreatorAuthority) || HasAuthority(sid, kAppPackageAuthority) ||
        Contains(others, sid)) {
      continue;
    }
    others.push_back(sid);
    const std::wstring text = SidText(sid);
    shapes.push_back(Shape{text, text, {everyone, sid}, 2});
  }

  for (const Shape& shape : shapes) {
    ACCESS_MASK unknown = 0;
    const ACCESS_MASK rights =
        RightsOf(dacl, owner, std::span<const PSID>(shape.set.data(), shape.count), &unknown) &
        mask;
    if (rights == 0) continue;
    if (rights & unknown) return BroadAccess{UnknownEntryName(dacl), rights};
    return BroadAccess{shape.name, rights, shape.sid};
  }
  return std::nullopt;
}

std::wstring UnsafeAccessReason(const UnsafeAccess& unsafe, bool accountNames) {
  switch (unsafe.kind) {
    case UnsafeKind::kNetwork:
      return L"is on a network drive";
    case UnsafeKind::kLink:
      return LinkWords(unsafe.reparseTag);
    case UnsafeKind::kAccess:
      break;
  }
  std::wstring who = unsafe.access.principal;
  if (!accountNames && unsafe.access.lookedUp && !unsafe.access.sid.empty()) {
    who = unsafe.access.sid;
  }
  if (!unsafe.access.sid.empty() && unsafe.access.sid != who) {
    who += L" (" + unsafe.access.sid + L")";
  }
  return (unsafe.access.owner ? L"is owned by " : L"can be changed by ") + who;
}

std::wstring UnsafeAccessText(const UnsafeAccess& unsafe) {
  return unsafe.object.native() + L" " + UnsafeAccessReason(unsafe);
}

bool IsOnNetworkDrive(const std::filesystem::path& path) noexcept {
  const std::wstring& text = path.native();
  const auto separator = [&](size_t at) {
    return at < text.size() && (text[at] == L'\\' || text[at] == L'/');
  };
  const auto driveAt = [&](size_t at) {
    return at + 1 < text.size() &&
           ((text[at] >= L'A' && text[at] <= L'Z') || (text[at] >= L'a' && text[at] <= L'z')) &&
           text[at + 1] == L':';
  };
  size_t drive = 0;
  if (separator(0) && separator(1)) {
    const bool device = text.size() > 3 && (text[2] == L'?' || text[2] == L'.') && separator(3);
    if (!device) return true;  // \\server\share
    if (IStartsWith(std::wstring_view(text).substr(4), L"UNC") && separator(7)) return true;
    // An object of the object manager, which may be \Device\Mup (a share) or a link to one:
    // nothing tells without opening it, so it does not count as local.
    if (IStartsWith(std::wstring_view(text).substr(4), L"GLOBALROOT") && separator(14)) {
      return true;
    }
    if (!driveAt(4)) return false;  // a device or a volume by its GUID: not a drive letter
    drive = 4;
  } else if (!driveAt(0)) {
    return false;
  }
  // The drive's type, without opening anything on it.
  const wchar_t root[] = {text[drive], L':', L'\\', L'\0'};
  return GetDriveTypeW(root) == DRIVE_REMOTE;
}

std::optional<UnsafeAccess> FindNetworkFolder(const std::filesystem::path& folder) {
  if (!IsOnNetworkDrive(folder)) return std::nullopt;
  UnsafeAccess unsafe;
  unsafe.object = folder;
  unsafe.kind = UnsafeKind::kNetwork;
  unsafe.access.principal = L"the computer that shares it (it is on a network drive)";
  return unsafe;
}

HANDLE HeldBuildFolder::File(const std::filesystem::path& relative) const noexcept {
  for (size_t i = 0; i < files_.size(); ++i) {
    if (IEquals(files_[i].native(), relative.native())) return fileHandles_[i].get();
  }
  return nullptr;
}

void HeldBuildFolder::Release() noexcept {
  fileHandles_.clear();
  folders_.clear();
  files_.clear();
  dir_.clear();
}

HRESULT HoldBuildFolder(const std::filesystem::path& folder, const BuildFolderHoldOptions& options,
                        HeldBuildFolder* held, std::optional<UnsafeAccess>* unsafe) noexcept try {
  held->Release();
  unsafe->reset();
  if (!folder.is_absolute()) RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  // Before anything is opened: even resolving a name would ask the other computer.
  if (std::optional<UnsafeAccess> network = FindNetworkFolder(folder)) {
    *unsafe = std::move(network);
    return S_OK;
  }
  std::filesystem::path dir;
  RETURN_IF_FAILED(PDFT_TRACE(DriveFolderPath(folder, &dir)));
  const auto validName = [](std::wstring_view name) { return IsPlainRelativeName(name); };
  if (!std::all_of(options.files.begin(), options.files.end(), validName) ||
      !std::all_of(options.optional.begin(), options.optional.end(), validName) ||
      (!options.tree.empty() && !validName(options.tree))) {
    RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  }

  // From the volume's root down, each folder held before the next is opened through it: a folder
  // that is held and not empty cannot be renamed, deleted or turned into a link. A failure's trace
  // detail is how many folders up, never a name: the build's folder may lie in the user's profile.
  std::vector<std::filesystem::path> chain;
  for (std::filesystem::path part = dir;; part = part.parent_path()) {
    chain.push_back(part);
    if (!part.has_relative_path()) break;
  }
  std::reverse(chain.begin(), chain.end());
  Gathered gathered;
  std::wstring heldPath;  // the final path of the folder last held (FinalNtPath)
  for (size_t i = 0; i < chain.size(); ++i) {
    const size_t up = chain.size() - 1 - i;
    const std::wstring detail = up == 0 ? std::wstring() : LevelsUp(up);
    wil::unique_hfile opened;
    if (const HRESULT hr = OpenHeld(chain[i], true, std::addressof(opened)); FAILED(hr)) {
      return PDFT_TRACE_DETAIL(hr, detail);
    }
    HeldKind kind = HeldKind::kOther;
    DWORD tag = 0;
    RETURN_IF_FAILED(PDFT_TRACE_DETAIL(Classify(opened.get(), &kind, &tag), detail));
    if (kind == HeldKind::kLink) {
      *unsafe = LinkAccess(chain[i], tag);
      return S_OK;
    }
    if (kind != HeldKind::kFolder) {
      RETURN_HR(PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_DIRECTORY), detail));
    }
    std::wstring own;
    if (i == 0) {
      RETURN_IF_FAILED(PDFT_TRACE_DETAIL(FinalNtPath(opened.get(), &own), detail));
    } else {
      bool inside = false;
      RETURN_IF_FAILED(PDFT_TRACE_DETAIL(LiesIn(opened.get(), heldPath, &own, &inside), detail));
      if (!inside) {
        *unsafe = LinkAccess(chain[i], 0);
        return S_OK;
      }
    }
    heldPath = std::move(own);
    HeldObject object{chain[i], detail,
                      up == 0 ? kBuildObjectChangeRights : kBuildParentChangeRights, opened.get()};
    if (up == 0) {
      gathered.judged.push_back(std::move(object));
    } else {
      gathered.above.insert(gathered.above.begin(), std::move(object));
    }
    gathered.folders.push_back(std::move(opened));
  }

  // What is copied from the folder: the files by their names (a missing optional one, or one
  // that is a folder or another kind of reparse point, is left out: it is not copied), then the
  // tree. A failure's trace detail is the file's or folder's own name.
  const auto holdItem = [&](std::wstring_view name, bool required) -> HRESULT {
    const std::filesystem::path relative(name);
    const std::filesystem::path item = dir / relative;
    wil::unique_hfile opened;
    if (const HRESULT hr = OpenHeld(item, false, std::addressof(opened)); FAILED(hr)) {
      const bool missing = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
      return (!required && missing) ? S_FALSE : hr;
    }
    HeldKind kind = HeldKind::kOther;
    DWORD tag = 0;
    RETURN_IF_FAILED(Classify(opened.get(), &kind, &tag));
    if (kind == HeldKind::kLink) {
      *unsafe = LinkAccess(item, tag);
      return S_OK;
    }
    if (kind != HeldKind::kFile) {
      return required ? HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE) : S_FALSE;
    }
    std::wstring own;
    bool inside = false;
    RETURN_IF_FAILED(LiesIn(opened.get(), heldPath, &own, &inside));
    if (!inside) {
      *unsafe = LinkAccess(item, 0);
      return S_OK;
    }
    gathered.judged.push_back(HeldObject{item, relative.filename().native(),
                                         kBuildObjectChangeRights & ~FILE_DELETE_CHILD,
                                         opened.get()});
    gathered.files.push_back(relative);
    gathered.fileHandles.push_back(std::move(opened));
    return S_OK;
  };
  for (const std::wstring_view name : options.files) {
    if (const HRESULT hr = holdItem(name, true); FAILED(hr)) {
      return PDFT_TRACE_DETAIL(hr, std::filesystem::path(name).filename().native());
    }
    if (*unsafe) return S_OK;
  }
  for (const std::wstring_view name : options.optional) {
    if (const HRESULT hr = holdItem(name, false); FAILED(hr)) {
      return PDFT_TRACE_DETAIL(hr, std::filesystem::path(name).filename().native());
    }
    if (*unsafe) return S_OK;
  }
  if (!options.tree.empty() && options.treeDepth > 0) {
    const std::filesystem::path relative(options.tree);
    const std::filesystem::path tree = dir / relative;
    const std::wstring detail = relative.filename().native();
    wil::unique_hfile opened;
    const HRESULT hr = OpenHeld(tree, false, std::addressof(opened));
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
        hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
      return PDFT_TRACE_DETAIL(hr, detail);
    }
    HeldKind kind = HeldKind::kOther;
    DWORD tag = 0;
    if (SUCCEEDED(hr)) {
      RETURN_IF_FAILED(PDFT_TRACE_DETAIL(Classify(opened.get(), &kind, &tag), detail));
    }
    if (kind == HeldKind::kLink) {
      *unsafe = LinkAccess(tree, tag);
      return S_OK;
    }
    // Copied only as a real folder, as before: a file of that name or another kind of reparse
    // point is left out.
    if (SUCCEEDED(hr) && kind == HeldKind::kFolder && tag == 0) {
      std::wstring own;
      bool inside = false;
      RETURN_IF_FAILED(PDFT_TRACE_DETAIL(LiesIn(opened.get(), heldPath, &own, &inside), detail));
      if (!inside) {
        *unsafe = LinkAccess(tree, 0);
        return S_OK;
      }
      const HANDLE handle = opened.get();
      gathered.judged.push_back(HeldObject{tree, detail, kBuildObjectChangeRights, handle});
      gathered.folders.push_back(std::move(opened));
      RETURN_IF_FAILED(
          HoldTree(dir, relative, handle, own, 1, options.treeDepth, &gathered, unsafe));
      if (*unsafe) return S_OK;
    }
  }

  // Everything is held: now nothing judged below can change before the copy is done. The folder
  // and what is copied first, then the folders above, the nearest first.
  if (options.checkAccess) {
    std::vector<std::vector<BYTE>> domainSids;
    RETURN_IF_FAILED(PDFT_TRACE(ThisPcsDomains(&domainSids)));
    std::vector<PSID> domains;
    for (std::vector<BYTE>& sid : domainSids) domains.push_back(sid.data());
    alignas(TOKEN_USER) BYTE tokenUser[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    wil::unique_hlocal givenUser;
    PSID user = nullptr;
    if (!options.user.empty()) {
      const std::wstring text(options.user);
      if (!ConvertStringSidToSidW(text.c_str(), givenUser.put())) {
        RETURN_HR(PDFT_TRACE(E_INVALIDARG));
      }
      user = givenUser.get();
    } else {
      DWORD size = 0;
      if (!GetTokenInformation(GetCurrentProcessToken(), TokenUser, tokenUser, sizeof(tokenUser),
                               &size)) {
        return PDFT_TRACE(LastErrorOr(E_FAIL));
      }
      user = reinterpret_cast<TOKEN_USER*>(tokenUser)->User.Sid;
    }
    gathered.judged.insert(gathered.judged.end(), gathered.above.begin(), gathered.above.end());
    for (const HeldObject& object : gathered.judged) {
      std::optional<BroadAccess> found;
      RETURN_IF_FAILED(PDFT_TRACE_DETAIL(
          JudgeAccess(object.handle, object.mask, user, domains, &found), object.detail));
      if (found) {
        NameAccount(&*found);
        *unsafe = UnsafeAccess{object.path, std::move(*found)};
        return S_OK;
      }
    }
  }

  held->dir_ = std::move(dir);
  held->files_ = std::move(gathered.files);
  held->fileHandles_ = std::move(gathered.fileHandles);
  held->folders_ = std::move(gathered.folders);
  return S_OK;
}
CATCH_RETURN();

HRESULT FindUnsafeBuildFolderAccess(const std::filesystem::path& folder,
                                    std::span<const std::wstring> binaries,
                                    std::optional<UnsafeAccess>* unsafe) noexcept {
  return FindUnsafeBuildFolderAccess(folder, binaries, {}, unsafe);
}

HRESULT FindUnsafeBuildFolderAccess(const std::filesystem::path& folder,
                                    std::span<const std::wstring> binaries,
                                    std::wstring_view userSid,
                                    std::optional<UnsafeAccess>* unsafe) noexcept try {
  unsafe->reset();
  if (std::optional<UnsafeAccess> network = FindNetworkFolder(folder)) {
    *unsafe = std::move(network);
    return S_OK;
  }
  std::filesystem::path canonical;
  RETURN_IF_FAILED(PDFT_TRACE(Canonicalize(folder, &canonical)));
  const std::vector<std::wstring_view> names(binaries.begin(), binaries.end());
  BuildFolderHoldOptions options;
  options.files = names;
  options.user = userSid;
  HeldBuildFolder held;
  return HoldBuildFolder(canonical, options, &held, unsafe);
}
CATCH_RETURN();

}  // namespace pdft
