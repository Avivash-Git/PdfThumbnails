#include "registration.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <wil/result_macros.h>

#include <cwchar>
#include <iterator>

#include "pe_info.h"
#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

constexpr wchar_t kClassesPrefix[] = L"Software\\Classes\\";
constexpr wchar_t kThreadingModel[] = L"ThreadingModel";

// The slots' file-type keys, relative to a root, in slot order.
constexpr const wchar_t* kSlotTypeKeys[kSlotCount] = {
    L"Software\\Classes\\.pdf",
    L"Software\\Classes\\SystemFileAssociations\\.pdf",
};

// Longest answer accepted from the association queries: a CLSID or a ProgID is far shorter.
constexpr DWORD kMaxAssocChars = 1024;

// Registry key names are at most 255 characters.
constexpr size_t kMaxKeyNameChars = 255;

std::wstring InprocKeyPath() { return ClassKeyPath() + L"\\InprocServer32"; }

// A string value, or std::nullopt when it is missing or of another type: for values whose wrong
// type means the same to Windows as no value.
HRESULT ReadStringOrNothing(HKEY root, std::wstring_view subKey, std::wstring_view name,
                            std::optional<std::wstring>* value) {
  const HRESULT hr = ReadRegistryString(root, subKey, name, value);
  if (hr == HRESULT_FROM_WIN32(ERROR_DATATYPE_MISMATCH)) return S_OK;
  return hr;
}

// A value of HKEY_CLASSES_ROOT\<subKey> as 1.x read it: the per-user value if there is one, else
// the machine-wide one.
HRESULT ReadClassesValue(const RegistryRoots& roots, std::wstring_view subKey,
                         std::wstring_view name, std::optional<std::wstring>* value) {
  const std::wstring path = std::wstring(kClassesPrefix) + std::wstring(subKey);
  RETURN_IF_FAILED(ReadStringOrNothing(roots.user, path, name, value));
  if (*value) return S_OK;
  return ReadStringOrNothing(roots.machine, path, name, value);
}

bool IsFile(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The PDFium file name next to a handler DLL of |fileVersion|: 1.x builds used the 1.x product
// name, which never changes; later builds use this build's name.
std::wstring PdfiumNameFor(std::wstring_view fileVersion) {
  if (fileVersion.starts_with(L"1.")) return std::wstring(kLegacyRootName) + L"-pdfium.dll";
  return kPdfiumDll;
}

// An absolute path without "." or ".." parts: the only kind of DLL path the app writes.
bool IsPlainAbsolutePath(const std::filesystem::path& path) {
  if (!path.is_absolute()) return false;
  for (const std::filesystem::path& part : path) {
    if (part == L"." || part == L"..") return false;
  }
  return true;
}

// A ProgID that can be looked up as a key name below Software\Classes.
bool IsKeyName(std::wstring_view name) noexcept {
  return !name.empty() && name.size() <= kMaxKeyNameChars &&
         name.find_first_of(std::wstring_view(L"\\\0", 2)) == std::wstring_view::npos;
}

// One AssocQueryStringW answer for .pdf; std::nullopt when Windows resolves nothing.
HRESULT AssocString(ASSOCSTR what, const wchar_t* extra, std::optional<std::wstring>* out) noexcept
    try {
  out->reset();
  DWORD length = 0;
  // Without a buffer it reports the size it needs (S_FALSE). Any failure means "nothing
  // resolved", as in 1.x.
  HRESULT hr = AssocQueryStringW(ASSOCF_NONE, what, kPdfExtension, extra, nullptr, &length);
  if (FAILED(hr) || length == 0 || length > kMaxAssocChars) return S_OK;
  std::wstring buffer(length, L'\0');
  hr = AssocQueryStringW(ASSOCF_NONE, what, kPdfExtension, extra, buffer.data(), &length);
  if (FAILED(hr)) return S_OK;
  buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
  if (!buffer.empty()) *out = std::move(buffer);
  return S_OK;
}
CATCH_RETURN();

class SystemQueries final : public AssociationQueries {
 public:
  HRESULT ThumbnailHandler(std::optional<std::wstring>* clsid) const noexcept override {
    return AssocString(ASSOCSTR_SHELLEXTENSION, kThumbSlotIidString, clsid);
  }
  HRESULT ProgId(std::optional<std::wstring>* progId) const noexcept override {
    return AssocString(ASSOCSTR_PROGID, nullptr, progId);
  }
};

}  // namespace

std::wstring ClassKeyPath() {
  return std::wstring(kClassesPrefix) + L"CLSID\\" + kHandlerClsidString;
}

HRESULT ReadClass(const RegistryRoots& roots, Scope scope, std::optional<ClassReg>* reg) noexcept
    try {
  reg->reset();
  const HKEY root = RootOf(roots, scope);
  RETURN_HR_IF(E_INVALIDARG, root == nullptr);
  const std::wstring inproc = InprocKeyPath();
  std::optional<RegistryValue> raw;
  RETURN_IF_FAILED(ReadRegistryValue(root, inproc, L"", &raw));
  if (!raw) return S_OK;
  ClassReg result;
  if (const std::optional<std::wstring> text = RegistryText(*raw)) {
    if (text->empty()) return S_OK;  // 1.x: an empty path is no registration
    if (raw->type == REG_EXPAND_SZ) {
      RETURN_IF_FAILED(ExpandEnvironment(*text, &result.dll));
    } else {
      result.dll = *text;
    }
  }
  std::optional<std::wstring> text;
  RETURN_IF_FAILED(ReadStringOrNothing(root, inproc, kThreadingModel, &text));
  result.threading = text.value_or(L"");
  RETURN_IF_FAILED(ReadStringOrNothing(root, ClassKeyPath(), L"", &text));
  result.name = text.value_or(L"");

  // Only an absolute path says which file Explorer loads; a relative one would be resolved
  // against whatever folder the loading process happens to run in.
  const std::filesystem::path dll(result.dll);
  result.dllExists = !result.dll.empty() && dll.is_absolute() && IsFile(dll);
  if (result.dllExists) {
    USHORT machine = 0;
    if (SUCCEEDED(ReadPeMachine(dll, &machine))) result.peMachine = machine;
    std::wstring version;
    if (SUCCEEDED(ReadFileVersionString(dll, &version))) result.fileVersion = std::move(version);
    result.pdfiumExists = IsFile(dll.parent_path() / PdfiumNameFor(result.fileVersion));
  }
  *reg = std::move(result);
  return S_OK;
}
CATCH_RETURN();

HRESULT WriteClass(const RegistryRoots& roots, Scope scope,
                   const std::filesystem::path& dll) noexcept try {
  RETURN_HR_IF(E_INVALIDARG, scope != Scope::kUser || !IsPlainAbsolutePath(dll));
  const HKEY root = RootOf(roots, scope);
  RETURN_HR_IF(E_INVALIDARG, root == nullptr);
  // 1.x's order: the class name, then the server and its threading model.
  RETURN_IF_FAILED(WriteRegistryString(root, ClassKeyPath(), L"", kHandlerClassName));
  const std::wstring inproc = InprocKeyPath();
  RETURN_IF_FAILED(WriteRegistryString(root, inproc, L"", dll.native()));
  RETURN_IF_FAILED(WriteRegistryString(root, inproc, kThreadingModel, kApartmentThreading));
  return S_OK;
}
CATCH_RETURN();

HRESULT DeleteClass(const RegistryRoots& roots, Scope scope) noexcept try {
  RETURN_HR_IF(E_INVALIDARG, scope != Scope::kUser);
  const HKEY root = RootOf(roots, scope);
  RETURN_HR_IF(E_INVALIDARG, root == nullptr);
  return DeleteRegistryTree(root, ClassKeyPath());
}
CATCH_RETURN();

std::wstring SlotKeyPath(size_t slot) {
  if (slot >= kSlotCount) return {};
  return std::wstring(kSlotTypeKeys[slot]) + L"\\ShellEx\\" + kThumbSlotIidString;
}

std::wstring_view SlotName(size_t slot) noexcept {
  if (slot >= kSlotCount) return {};
  return std::wstring_view(kSlotTypeKeys[slot]).substr(std::size(kClassesPrefix) - 1);
}

bool IsOurClsid(std::wstring_view value) noexcept { return IEquals(value, kHandlerClsidString); }

bool SlotIsEmpty(const std::optional<std::wstring>& value) noexcept {
  return !value || value->empty();
}

HRESULT ReadSlots(const RegistryRoots& roots, SlotValues* user, SlotValues* machine) noexcept try {
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    (*user)[slot].reset();
    if (machine) (*machine)[slot].reset();
  }
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    const std::wstring path = SlotKeyPath(slot);
    RETURN_IF_FAILED(ReadStringOrNothing(roots.user, path, L"", &(*user)[slot]));
    if (machine) RETURN_IF_FAILED(ReadStringOrNothing(roots.machine, path, L"", &(*machine)[slot]));
  }
  return S_OK;
}
CATCH_RETURN();

bool SlotNeedsWrite(const std::optional<std::wstring>& current, SlotWrite mode) noexcept {
  switch (mode) {
    case SlotWrite::kFillEmpty:
      return SlotIsEmpty(current);
    case SlotWrite::kTakeBack:
      return SlotIsEmpty(current) || !IsOurClsid(*current);
  }
  return false;
}

HRESULT WriteSlots(const RegistryRoots& roots, SlotWrite mode,
                   std::array<bool, kSlotCount>* changed) noexcept try {
  if (changed) changed->fill(false);
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    const std::wstring path = SlotKeyPath(slot);
    std::optional<std::wstring> current;
    RETURN_IF_FAILED(ReadStringOrNothing(roots.user, path, L"", &current));
    if (!SlotNeedsWrite(current, mode)) continue;
    RETURN_IF_FAILED(WriteRegistryString(roots.user, path, L"", kHandlerClsidString));
    if (changed) (*changed)[slot] = true;
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT RemoveSlotsIfOurs(const RegistryRoots& roots,
                          std::array<bool, kSlotCount>* removed) noexcept try {
  if (removed) removed->fill(false);
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    const std::wstring path = SlotKeyPath(slot);
    std::optional<std::wstring> current;
    RETURN_IF_FAILED(ReadStringOrNothing(roots.user, path, L"", &current));
    if (!current || !IsOurClsid(*current)) continue;  // someone else's: theirs to remove
    bool deleted = false;
    RETURN_IF_FAILED(DeleteRegistryTree(roots.user, path, &deleted));
    if (removed) (*removed)[slot] = deleted;
  }
  for (const wchar_t* parent : kSlotParentKeys) {
    RETURN_IF_FAILED(DeleteRegistryKeyIfEmpty(roots.user, parent));
  }
  return S_OK;
}
CATCH_RETURN();

bool IsGuidString(std::wstring_view text) noexcept {
  if (text.size() != 38 || text.front() != L'{' || text.back() != L'}') return false;
  for (size_t i = 1; i < 37; ++i) {
    const wchar_t c = text[i];
    if (i == 9 || i == 14 || i == 19 || i == 24) {
      if (c != L'-') return false;
    } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                 (c >= L'A' && c <= L'F'))) {
      return false;
    }
  }
  return true;
}

HRESULT DescribeClsid(const RegistryRoots& roots, std::wstring_view clsid,
                      ClsidDescription* description) noexcept try {
  description->name.reset();
  description->dll.reset();
  if (!IsGuidString(clsid)) return S_OK;
  const std::wstring key = L"CLSID\\" + std::wstring(clsid);
  RETURN_IF_FAILED(ReadClassesValue(roots, key, L"", &description->name));
  RETURN_IF_FAILED(ReadClassesValue(roots, key + L"\\InprocServer32", L"", &description->dll));
  return S_OK;
}
CATCH_RETURN();

const AssociationQueries& SystemAssociationQueries() noexcept {
  static const SystemQueries queries;
  return queries;
}

HRESULT QueryEffectiveHandler(const AssociationQueries& queries,
                              std::optional<std::wstring>* clsid) noexcept {
  return queries.ThumbnailHandler(clsid);
}

HRESULT QueryDefaultApp(const RegistryRoots& roots, const AssociationQueries& queries,
                        DefaultApp* app) noexcept try {
  *app = {};
  RETURN_IF_FAILED(queries.ProgId(&app->progId));
  if (!app->progId || !IsKeyName(*app->progId)) return S_OK;
  const std::wstring& progId = *app->progId;
  RETURN_IF_FAILED(
      ReadClassesValue(roots, progId + L"\\ShellEx\\" + kThumbSlotIidString, L"", &app->handler));
  RETURN_IF_FAILED(ReadClassesValue(roots, progId, L"TypeOverlay", &app->overlay));
  return S_OK;
}
CATCH_RETURN();

bool NotifyAssocChanged(const RegistryRoots& roots) noexcept {
  if (!UsesSystemRoots(roots)) return false;
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return true;
}

}  // namespace pdft
