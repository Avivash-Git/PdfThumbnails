#include "file_ops.h"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "elevation.h"
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

// ---- the broad groups ----

constexpr ACCESS_MASK kOwnerImplicitRights = READ_CONTROL | WRITE_DAC;

struct WellKnownSid {
  BYTE bytes[SECURITY_MAX_SID_SIZE] = {};
  PSID sid() { return bytes; }
};

bool Contains(std::span<const PSID> set, PSID sid) noexcept {
  if (!sid || !IsValidSid(sid)) return false;
  return std::any_of(set.begin(), set.end(), [sid](PSID member) { return EqualSid(member, sid); });
}

// The rights a token holding exactly the groups |set| gets from |dacl| (owned by |owner|),
// evaluated the way Windows does: entries in order, each right decided by the first entry that
// grants or denies it.
ACCESS_MASK RightsOf(PACL dacl, PSID owner, std::span<const PSID> set) noexcept {
  GENERIC_MAPPING mapping = {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                             FILE_ALL_ACCESS};
  ACCESS_MASK granted = 0;
  ACCESS_MASK denied = 0;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* ace = nullptr;
    if (!GetAce(dacl, index, &ace)) break;
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    // Every access entry type starts with the header and the mask.
    ACCESS_MASK mask = static_cast<const ACCESS_ALLOWED_ACE*>(ace)->Mask;
    MapGenericMask(&mask, &mapping);
    // The allowed, denied and callback forms keep the SID right after the mask.
    PSID sid = const_cast<DWORD*>(&static_cast<const ACCESS_ALLOWED_ACE*>(ace)->SidStart);
    switch (header->AceType) {
      case ACCESS_ALLOWED_ACE_TYPE:
      case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:  // as if its condition held
        if (Contains(set, sid)) granted |= mask & ~denied;
        break;
      case ACCESS_DENIED_ACE_TYPE:
        if (Contains(set, sid)) denied |= mask & ~granted;
        break;
      case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
      case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        // Not used on files; their SID is not parsed here, so they count for everyone.
        granted |= mask & ~denied;
        break;
      default:  // a deny with a condition counts as if the condition did not hold
        break;
    }
  }
  if (Contains(set, owner)) granted |= kOwnerImplicitRights;
  return granted;
}

}  // namespace

HRESULT CheckNoReparsePoints(const std::filesystem::path& path,
                             const std::filesystem::path& stopAt) noexcept try {
  RETURN_HR_IF(E_INVALIDARG, !path.is_absolute() || (!stopAt.empty() && !stopAt.is_absolute()));
  const std::filesystem::path target = NormalPath(path);
  const std::filesystem::path stop = stopAt.empty() ? target.root_path() : NormalPath(stopAt);
  std::vector<std::filesystem::path> chain;
  for (std::filesystem::path part = target;; part = part.parent_path()) {
    chain.push_back(part);
    if (IEquals(part.native(), stop.native())) break;
    // At the root without meeting |stopAt|: it is not above |path|.
    RETURN_HR_IF(E_INVALIDARG, !part.has_relative_path());
  }
  // From the top down: below a missing folder nothing exists either.
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const DWORD attributes = GetFileAttributesW(it->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = GetLastError();
      if (IsMissingError(error)) return S_OK;
      return HRESULT_FROM_WIN32(error);
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      return HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
    }
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT CopyFileVerified(const std::filesystem::path& from, const std::filesystem::path& to,
                         Sha256Digest* sha) noexcept try {
  if (sha) sha->fill(0);
  RETURN_HR_IF(E_INVALIDARG, !from.is_absolute() || !to.is_absolute() || !to.has_filename());
  const std::filesystem::path folder = NormalPath(to.parent_path());
  const DWORD folderAttributes = GetFileAttributesW(folder.c_str());
  if (folderAttributes == INVALID_FILE_ATTRIBUTES) return LastErrorOr(E_FAIL);
  if (folderAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  }
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_DIRECTORY), !(folderAttributes & FILE_ATTRIBUTE_DIRECTORY));

  // Read sharing only: nobody can change or delete the source while it is copied, and a
  // process that already has it open for writing makes this open fail.
  wil::unique_hfile source(
      CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!source) return LastErrorOr(E_FAIL);
  RETURN_IF_FAILED(CheckRegularFile(source.get()));

  std::wstring unique;
  RETURN_IF_FAILED(NewHexName(&unique));
  const std::filesystem::path temporary =
      folder / (to.filename().native() + L"." + unique + L".tmp");
  wil::unique_hfile target(CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0,
                                       nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!target) return LastErrorOr(E_FAIL);
  // From here on every failure deletes the temporary file again.
  auto discard = wil::scope_exit([&]() noexcept { DeleteOnClose(target.get()); });

  std::vector<BYTE> buffer(kCopyChunkBytes);
  Sha256Digest copied = {};
  RETURN_IF_FAILED(Pump(source.get(), target.get(), &buffer, &copied));
  RETURN_IF_WIN32_BOOL_FALSE(FlushFileBuffers(target.get()));
  LARGE_INTEGER start = {};
  RETURN_IF_WIN32_BOOL_FALSE(SetFilePointerEx(target.get(), start, nullptr, FILE_BEGIN));
  Sha256Digest written = {};
  RETURN_IF_FAILED(Pump(target.get(), nullptr, &buffer, &written));
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_DATA_CHECKSUM_ERROR), written != copied);

  RETURN_IF_FAILED(RenameByHandle(target.get(), NormalPath(to)));
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
  // Others may go on reading it; nobody may write, rename or delete it while it is judged.
  wil::unique_hfile handle(CreateFileW(
      file.c_str(), DELETE | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN,
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
  FILE_DISPOSITION_INFO_EX dispositionEx = {};
  dispositionEx.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
  if (!SetFileInformationByHandle(handle.get(), FileDispositionInfoEx, &dispositionEx,
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
    if (GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
      basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
      if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
      SetFileInformationByHandle(handle.get(), FileBasicInfo, &basic, sizeof(basic));
    }
    FILE_DISPOSITION_INFO disposition = {};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition,
                                    sizeof(disposition))) {
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
CATCH_RETURN();

std::optional<BroadAccess> FindBroadAccess(PSECURITY_DESCRIPTOR descriptor, ACCESS_MASK mask) {
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  PSID owner = nullptr;
  if (!descriptor || !GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) ||
      !GetSecurityDescriptorOwner(descriptor, &owner, &defaulted)) {
    return BroadAccess{L"Everyone", mask};  // unreadable: assume the worst
  }
  if (!present || !dacl) return BroadAccess{L"Everyone", mask};  // a null DACL grants everything

  std::array<WellKnownSid, 4> sids;
  constexpr WELL_KNOWN_SID_TYPE kTypes[] = {WinWorldSid, WinAuthenticatedUserSid,
                                            WinBuiltinUsersSid, WinInteractiveSid};
  for (size_t i = 0; i < sids.size(); ++i) {
    DWORD size = SECURITY_MAX_SID_SIZE;
    if (!CreateWellKnownSid(kTypes[i], nullptr, sids[i].sid(), &size)) {
      return BroadAccess{L"Everyone", mask};
    }
  }
  const PSID everyone = sids[0].sid();
  struct Shape {
    const wchar_t* name;
    std::array<PSID, 4> set;
    size_t count;
  };
  const Shape shapes[] = {
      {L"Everyone", {everyone}, 1},
      {L"Authenticated Users", {everyone, sids[1].sid()}, 2},
      {L"Users", {everyone, sids[2].sid()}, 2},
      {L"INTERACTIVE", {everyone, sids[3].sid()}, 2},
      {L"Authenticated Users, Users and INTERACTIVE together",
       {everyone, sids[1].sid(), sids[2].sid(), sids[3].sid()},
       4},
  };
  for (const Shape& shape : shapes) {
    const ACCESS_MASK rights =
        RightsOf(dacl, owner, std::span<const PSID>(shape.set.data(), shape.count)) & mask;
    if (rights != 0) return BroadAccess{shape.name, rights};
  }
  return std::nullopt;
}

HRESULT FindUnsafeBuildFolderAccess(const std::filesystem::path& folder,
                                    std::span<const std::wstring> binaries,
                                    std::optional<UnsafeAccess>* unsafe) noexcept try {
  unsafe->reset();
  std::filesystem::path canonical;
  RETURN_IF_FAILED(Canonicalize(folder, &canonical));
  canonical = NormalPath(canonical);

  const auto check = [&](const std::filesystem::path& object, ACCESS_MASK mask) -> HRESULT {
    wil::unique_hlocal_security_descriptor descriptor;
    const DWORD error = GetNamedSecurityInfoW(
        object.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, descriptor.put());
    RETURN_IF_WIN32_ERROR(error);
    if (std::optional<BroadAccess> found = FindBroadAccess(descriptor.get(), mask)) {
      *unsafe = UnsafeAccess{object, std::move(*found)};
    }
    return S_OK;
  };

  RETURN_IF_FAILED(check(canonical, kBuildObjectChangeRights));
  if (*unsafe) return S_OK;
  for (const std::wstring& name : binaries) {
    const std::filesystem::path file = NormalPath(canonical / name);
    RETURN_HR_IF(E_INVALIDARG, name.empty() || !IsUnderLexically(file, canonical));
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return LastErrorOr(E_FAIL);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE),
                 (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0);
    RETURN_IF_FAILED(check(file, kBuildObjectChangeRights & ~FILE_DELETE_CHILD));
    if (*unsafe) return S_OK;
  }
  for (std::filesystem::path parent = canonical; parent.has_relative_path();) {
    parent = parent.parent_path();
    RETURN_IF_FAILED(check(parent, kBuildParentChangeRights));
    if (*unsafe) return S_OK;
  }
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
