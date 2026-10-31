// The handler's registration: its COM class (per user or machine-wide) and the two per-user
// ".pdf" thumbnail slots that point Explorer at it, plus what Windows itself resolves for .pdf.
//
//   <root>\Software\Classes\CLSID\<handler CLSID>               (default) = class name
//                                   \InprocServer32              (default) = DLL path
//                                                                ThreadingModel = Apartment
//   HKCU\Software\Classes\.pdf\ShellEx\<IThumbnailProvider IID>  (default) = handler CLSID
//   HKCU\Software\Classes\SystemFileAssociations\.pdf\ShellEx\<IThumbnailProvider IID>
//
// Explorer reads the first slot; the second survives tools that delete the first on uninstall
// without checking whose it is. Every access goes through RegistryRoots (registry_roots.h). The
// two questions only Windows can answer (which handler it resolves for .pdf, and the default
// app's ProgID) ignore those roots, so they sit behind AssociationQueries, which tests replace.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "exit_code.h"
#include "registry_roots.h"

namespace pdft {

// The file type whose thumbnails the handler provides.
inline constexpr wchar_t kPdfExtension[] = L".pdf";

// The class's threading model, as 1.x wrote it and Explorer needs it.
inline constexpr wchar_t kApartmentThreading[] = L"Apartment";

// Software\Classes\CLSID\<handler CLSID>, relative to either root. May throw std::bad_alloc.
std::wstring ClassKeyPath();

struct ClassReg {
  std::wstring name;          // the class key's default value; empty when it has none
  std::wstring dll;           // InprocServer32's default value (REG_EXPAND_SZ expanded); empty when
                              // that value is not a string
  std::wstring threading;     // ThreadingModel; empty when missing
  bool dllExists = false;     // |dll| is an absolute path to a file
  bool pdfiumExists = false;  // and the PDFium DLL is next to it
  std::optional<USHORT> peMachine;  // the DLL's PE machine, when it could be read
  std::wstring fileVersion;         // its FileVersion, when it could be read
};

// The class registered in |scope| (kUser: roots.user, kMachine: roots.machine), or std::nullopt
// when InprocServer32 has no default value, as 1.x decided. A default value of the wrong type
// counts as registered but broken (dll empty, dllExists false). Reads the DLL's headers and
// version without loading it (pe_info.h).
[[nodiscard]] HRESULT ReadClass(const RegistryRoots& roots, Scope scope,
                                std::optional<ClassReg>* reg) noexcept;

// Registers the class per user, exactly as 1.x did: the class name, the DLL path |dll| (absolute,
// REG_SZ) and ThreadingModel=Apartment. Only Scope::kUser: the machine-wide class is written by
// the elevated steps, which compute its path themselves (E_INVALIDARG for any other scope).
[[nodiscard]] HRESULT WriteClass(const RegistryRoots& roots, Scope scope,
                                 const std::filesystem::path& dll) noexcept;

// Deletes the per-user class key with everything below it; nothing there is not an error.
// Only Scope::kUser, like WriteClass.
[[nodiscard]] HRESULT DeleteClass(const RegistryRoots& roots, Scope scope) noexcept;

// The slots: [0] .pdf, [1] SystemFileAssociations\.pdf. Each holds the default value of its key,
// or std::nullopt when the key or the value is missing or is not a string (1.x read those as
// empty, and Windows cannot use them either).
inline constexpr size_t kSlotCount = 2;
using SlotValues = std::array<std::optional<std::wstring>, kSlotCount>;

// Software\Classes\<.pdf or SystemFileAssociations\.pdf>\ShellEx\<IThumbnailProvider IID>,
// relative to either root (pure; may throw std::bad_alloc).
std::wstring SlotKeyPath(size_t slot);

// The slot's short name for messages: ".pdf" or "SystemFileAssociations\.pdf" (pure).
std::wstring_view SlotName(size_t slot) noexcept;

// The value names the handler: its CLSID, compared ignoring case like 1.x did (pure).
bool IsOurClsid(std::wstring_view value) noexcept;

// A slot without a usable value: missing or empty (pure).
bool SlotIsEmpty(const std::optional<std::wstring>& value) noexcept;

// Reads both per-user slots into |user| and, for information only, the machine-wide ones (the
// same paths under roots.machine) into |machine| (optional).
[[nodiscard]] HRESULT ReadSlots(const RegistryRoots& roots, SlotValues* user,
                                SlotValues* machine = nullptr) noexcept;

// How WriteSlots treats a slot that names something else.
enum class SlotWrite {
  kFillEmpty,  // only empty slots are written; another handler is left alone (the user may
               // have chosen it)
  kTakeBack,   // every slot that is not ours is written
};

// Whether WriteSlots writes a slot that holds |current| (pure).
bool SlotNeedsWrite(const std::optional<std::wstring>& current, SlotWrite mode) noexcept;

// Points the per-user slots at the handler as |mode| allows. |changed| (optional) says which
// slots were written.
[[nodiscard]] HRESULT WriteSlots(const RegistryRoots& roots, SlotWrite mode,
                                 std::array<bool, kSlotCount>* changed = nullptr) noexcept;

// The per-user keys above the slots, in the order 1.x pruned them (deepest first, .pdf before
// SystemFileAssociations), relative to roots.user.
inline constexpr const wchar_t* kSlotParentKeys[] = {
    L"Software\\Classes\\.pdf\\ShellEx",
    L"Software\\Classes\\.pdf",
    L"Software\\Classes\\SystemFileAssociations\\.pdf\\ShellEx",
    L"Software\\Classes\\SystemFileAssociations\\.pdf",
    L"Software\\Classes\\SystemFileAssociations",
};

// Deletes each per-user slot key that names the handler (a slot naming something else stays),
// then each of kSlotParentKeys that is left without subkeys and values, in that order. A key
// holding only a default value is not empty. |removed| (optional) says which slots went.
[[nodiscard]] HRESULT RemoveSlotsIfOurs(const RegistryRoots& roots,
                                        std::array<bool, kSlotCount>* removed = nullptr) noexcept;

// "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" with hex digits of either case (pure). Values read
// from slots and ProgIDs are checked with it before they become part of a registry path.
bool IsGuidString(std::wstring_view text) noexcept;

// What the registry says about any class, for display: its name and DLL, each looked up as
// HKEY_CLASSES_ROOT shows it (the per-user value if there is one, else the machine-wide one),
// as 1.x did. The class is only described, never loaded. Both stay std::nullopt for a |clsid|
// that is not IsGuidString.
struct ClsidDescription {
  std::optional<std::wstring> name;
  std::optional<std::wstring> dll;
};
[[nodiscard]] HRESULT DescribeClsid(const RegistryRoots& roots, std::wstring_view clsid,
                                    ClsidDescription* description) noexcept;

// The answers only Windows' association code has. They describe the whole system as this
// process sees it and ignore RegistryRoots. Each gives std::nullopt when Windows resolves
// nothing (1.x treated every failure that way).
class AssociationQueries {
 public:
  virtual ~AssociationQueries() = default;
  // The CLSID of the thumbnail handler for .pdf (AssocQueryStringW ASSOCSTR_SHELLEXTENSION with
  // the IThumbnailProvider IID).
  [[nodiscard]] virtual HRESULT ThumbnailHandler(
      std::optional<std::wstring>* clsid) const noexcept = 0;
  // The ProgID of .pdf's default app (ASSOCSTR_PROGID). Windows decides it from the user's
  // choice and the registered apps; the app never reads the UserChoice key itself.
  [[nodiscard]] virtual HRESULT ProgId(std::optional<std::wstring>* progId) const noexcept = 0;
};

// The queries as Windows answers them.
const AssociationQueries& SystemAssociationQueries() noexcept;

// The thumbnail handler Windows resolves for .pdf.
[[nodiscard]] HRESULT QueryEffectiveHandler(const AssociationQueries& queries,
                                            std::optional<std::wstring>* clsid) noexcept;

// The default app for .pdf: its ProgID (from |queries|), the thumbnail handler registered on that
// ProgID (<ProgID>\ShellEx\<IThumbnailProvider IID>, which outranks the .pdf slots), and its
// TypeOverlay value (std::nullopt: none, so Windows draws the app's icon on thumbnails; "":
// no overlay). Both read as HKEY_CLASSES_ROOT shows them through |roots|. A ProgID that could
// not be a key name (empty, too long, or with a backslash) is reported but not looked up.
struct DefaultApp {
  std::optional<std::wstring> progId;
  std::optional<std::wstring> handler;
  std::optional<std::wstring> overlay;
};
[[nodiscard]] HRESULT QueryDefaultApp(const RegistryRoots& roots, const AssociationQueries& queries,
                                      DefaultApp* app) noexcept;

// Tells Explorer that file associations changed (SHChangeNotify SHCNE_ASSOCCHANGED), so it picks
// up a changed slot or class. Only for the real roots (UsesSystemRoots): a test's sandbox
// changes nothing Explorer reads. True when the notification was sent.
bool NotifyAssocChanged(const RegistryRoots& roots) noexcept;

}  // namespace pdft
