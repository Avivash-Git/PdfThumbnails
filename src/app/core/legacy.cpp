#include "legacy.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <string_view>

#include "paths.h"
#include "project_config.h"
#include "settings.h"
#include "strings.h"

namespace pdft {
namespace {

// 1.x names that 2.x never uses (the 1.x name itself comes from project_config.h).
constexpr wchar_t kSettingsIni[] = L"settings.ini";
constexpr const wchar_t* kV1OnlyFiles[] = {L"settings.ini", L"repair.log", L"repair.log.old",
                                           L"elevated.log"};
constexpr wchar_t kBinFolder[] = L"bin";
constexpr wchar_t kAppFolder[] = L"app";
constexpr wchar_t kLicensesFolder[] = L"licenses";
constexpr wchar_t kV1BuildPrefix[] = L"1.";

// Copied by the 1.x manager next to itself, and names a later install could plausibly use too.
constexpr const wchar_t* kV1ManagerDocs[] = {L"README.md", L"LICENSE"};

// The license files 1.x put in licenses\ (its PDFium build's notices).
constexpr const wchar_t* kV1LicenseFiles[] = {
    L"abseil.txt",
    L"agg23.txt",
    L"dragonbox-Apache2-LLVM.txt",
    L"dragonbox-Boost.txt",
    L"fast_float.txt",
    L"freetype.txt",
    L"harfbuzz.txt",
    L"highway.txt",
    L"icu.txt",
    L"lcms.txt",
    L"libjpeg_turbo.ijg",
    L"libjpeg_turbo.md",
    L"libopenjpeg.txt",
    L"libpng.txt",
    L"llvm-libc.txt",
    L"pdfium-binaries-LICENSE.txt",
    L"pdfium.txt",
    L"simdutf.txt",
    L"zlib.txt",
};

// Root files of a 2.x install (cmake/Install.cmake's runtime tree, plus an installer's
// uninstaller, unins000.exe and its data): while one is there, licenses\ may be 2.x's own.
constexpr const wchar_t* k2xRootFiles[] = {L"LICENSE.txt", L"THIRD-PARTY-NOTICES.md"};
constexpr wchar_t k2xUninstallerPrefix[] = L"unins";

template <size_t N>
bool IsOneOf(std::wstring_view name, const wchar_t* const (&names)[N]) noexcept {
  return std::any_of(std::begin(names), std::end(names),
                     [&](const wchar_t* candidate) { return IEquals(name, candidate); });
}

bool IsFile(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The entries of |dir| (not "." and ".."); a missing folder has none.
HRESULT ListFolder(const std::filesystem::path& dir, std::vector<FolderEntry>* entries) {
  entries->clear();
  WIN32_FIND_DATAW data = {};
  wil::unique_hfind find(FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic, &data,
                                          FindExSearchNameMatch, nullptr,
                                          FIND_FIRST_EX_LARGE_FETCH));
  if (!find) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_OK;
    RETURN_WIN32(error);
  }
  do {
    const std::wstring_view name = data.cFileName;
    if (name == L"." || name == L"..") continue;
    entries->push_back({std::wstring(name), (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                        (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
  } while (FindNextFileW(find.get(), &data));
  const DWORD error = GetLastError();
  RETURN_HR_IF(HRESULT_FROM_WIN32(error), error != ERROR_NO_MORE_FILES);
  return S_OK;
}

HRESULT KnownFolderChild(REFKNOWNFOLDERID folder, std::wstring_view name,
                         std::filesystem::path* out) {
  out->clear();
  wil::unique_cotaskmem_string base;
  RETURN_IF_FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &base));
  *out = std::filesystem::path(base.get()) / name;
  return S_OK;
}

}  // namespace

bool HasV1Leftovers(const LegacyState& state) noexcept {
  // A machine-wide class in a 1.x build is not counted: it is the install File Explorer uses,
  // which installing 2.0 for all users replaces, and nothing removes it as a leftover.
  return state.runOnce || state.legacyTask || state.ini || !state.files.empty() ||
         state.userClassInV1;
}

std::vector<std::wstring> ClassifyV1Files(const UserFolderListing& listing) {
  const std::wstring legacyName = kLegacyRootName;
  const std::wstring script = legacyName + L".ps1";
  const std::wstring launcher = legacyName + L".cmd";

  const auto hasFile = [&](std::wstring_view name) {
    return std::any_of(listing.root.begin(), listing.root.end(), [&](const FolderEntry& entry) {
      return !entry.directory && !entry.reparsePoint && IEquals(entry.name, name);
    });
  };
  // The 1.x manager is still there: its own documents next to it are 1.x's too.
  const bool manager = hasFile(script) || hasFile(launcher);
  // Anything of a 2.x install, whatever its kind: then licenses\ may be 2.x's.
  const bool has2x =
      std::any_of(listing.root.begin(), listing.root.end(), [&](const FolderEntry& entry) {
        return IEquals(entry.name, kSettingsExe) || IEquals(entry.name, kCliExe) ||
               IsOneOf(entry.name, k2xRootFiles) || IStartsWith(entry.name, k2xUninstallerPrefix);
      });
  // Only files that 1.x shipped, no subfolders (2.x keeps PDFium's notices in licenses\pdfium\).
  // A partly emptied folder still counts, so an interrupted clean-up is finished later.
  const bool licensesAre1x =
      std::all_of(listing.licenses.begin(), listing.licenses.end(), [](const FolderEntry& entry) {
        return !entry.directory && !entry.reparsePoint && IsOneOf(entry.name, kV1LicenseFiles);
      });

  std::vector<std::wstring> found;
  bool hasAppFolder = false;
  for (const FolderEntry& entry : listing.root) {
    if (entry.reparsePoint) continue;  // never followed, never counted
    if (!entry.directory) {
      if (IEquals(entry.name, script) || IEquals(entry.name, launcher) ||
          IsOneOf(entry.name, kV1OnlyFiles) || (manager && IsOneOf(entry.name, kV1ManagerDocs))) {
        found.push_back(entry.name);
      }
    } else if (IEquals(entry.name, kBinFolder)) {
      found.push_back(entry.name);
    } else if (IEquals(entry.name, kLicensesFolder)) {
      if (manager && !has2x && licensesAre1x) found.push_back(entry.name);
    } else if (IEquals(entry.name, kAppFolder)) {
      hasAppFolder = true;
    }
  }
  if (hasAppFolder) {
    for (const FolderEntry& entry : listing.app) {
      if (entry.directory && !entry.reparsePoint && IStartsWith(entry.name, kV1BuildPrefix)) {
        found.push_back(std::wstring(kAppFolder) + L"\\" + entry.name);
      }
    }
  }
  std::sort(found.begin(), found.end(), [](const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
  });
  return found;
}

bool IsInV1BuildFolder(const std::filesystem::path& dll,
                       const std::filesystem::path& root) noexcept {
  try {
    if (dll.empty() || root.empty()) return false;
    const std::filesystem::path app = (root / kAppFolder).lexically_normal();
    if (!IsUnderLexically(dll, app)) return false;
    // Climb from the DLL to the folder directly inside app\; the DLL must be inside it.
    const std::filesystem::path file = dll.lexically_normal();
    std::filesystem::path child = file;
    for (std::filesystem::path parent = child.parent_path(); parent != child;
         child = parent, parent = parent.parent_path()) {
      if (IEquals(parent.native(), app.native())) {
        return child != file && IStartsWith(child.filename().native(), kV1BuildPrefix);
      }
    }
    return false;
  } catch (...) {
    return false;  // out of memory: "not a 1.x folder" reports less, and changes nothing
  }
}

HRESULT GetLegacyUserRoot(std::filesystem::path* root) noexcept try {
  return KnownFolderChild(FOLDERID_UserProgramFiles, kLegacyRootName, root);
}
CATCH_RETURN();

HRESULT GetLegacyMachineRoot(std::filesystem::path* root) noexcept try {
  return KnownFolderChild(FOLDERID_ProgramFiles, kLegacyRootName, root);
}
CATCH_RETURN();

HRESULT ListUserFolder(const std::filesystem::path& root, UserFolderListing* listing) noexcept try {
  *listing = {};
  const DWORD attributes = GetFileAttributesW(root.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return S_OK;  // nothing there, or a link that is not ours to look into
  }
  RETURN_IF_FAILED(ListFolder(root, &listing->root));
  for (const FolderEntry& entry : listing->root) {
    if (!entry.directory || entry.reparsePoint) continue;
    if (IEquals(entry.name, kAppFolder)) {
      RETURN_IF_FAILED(ListFolder(root / entry.name, &listing->app));
    } else if (IEquals(entry.name, kLicensesFolder)) {
      RETURN_IF_FAILED(ListFolder(root / entry.name, &listing->licenses));
    }
  }
  return S_OK;
}
CATCH_RETURN();

void MarkV1Classes(const std::optional<ClassReg>& userClass,
                   const std::optional<ClassReg>& machineClass,
                   const std::filesystem::path& userRoot, const std::filesystem::path& machineRoot,
                   LegacyState* state) noexcept {
  const auto inV1 = [&](const std::optional<ClassReg>& reg) {
    return reg &&
           (IsInV1BuildFolder(reg->dll, userRoot) || IsInV1BuildFolder(reg->dll, machineRoot));
  };
  state->userClassInV1 = inV1(userClass);
  state->machineClassInV1 = inV1(machineClass);
}

HRESULT ReadLegacyState(const RegistryRoots& roots, const std::filesystem::path& userRoot,
                        const std::filesystem::path& machineRoot, LegacyState* state) noexcept try {
  *state = {};
  std::optional<RegistryValue> runOnce;
  RETURN_IF_FAILED(ReadRegistryValue(roots.user, kRunOnceKey, kLegacyRunOnce, &runOnce));
  state->runOnce = runOnce.has_value();

  const std::filesystem::path ini = userRoot / kSettingsIni;
  state->ini = IsFile(ini);
  RETURN_IF_FAILED(ReadLegacyIniZoom(ini, &state->iniZoom));

  UserFolderListing listing;
  RETURN_IF_FAILED(ListUserFolder(userRoot, &listing));
  for (const std::wstring& relative : ClassifyV1Files(listing)) {
    state->files.push_back(userRoot / relative);
  }

  std::optional<ClassReg> userClass;
  std::optional<ClassReg> machineClass;
  RETURN_IF_FAILED(ReadClass(roots, Scope::kUser, &userClass));
  RETURN_IF_FAILED(ReadClass(roots, Scope::kMachine, &machineClass));
  MarkV1Classes(userClass, machineClass, userRoot, machineRoot, state);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
