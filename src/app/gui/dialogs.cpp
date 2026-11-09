#include "dialogs.h"

#include <commctrl.h>

#include "project_config.h"
#include "resource_ids.h"

namespace pdft {
namespace {

std::wstring Text(UINT id) {
  const wchar_t* text = nullptr;
  const int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
  return length > 0 && text ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
}

}  // namespace

bool ConfirmTakeBack(HWND owner, std::span<const std::wstring> slots) {
  std::wstring title = Text(IDS_TAKE_BACK_TITLE);
  constexpr std::wstring_view kProductField = L"{product}";
  if (const size_t at = title.find(kProductField); at != std::wstring::npos) {
    title.replace(at, kProductField.size(), kProductName);
  }
  std::wstring content = Text(IDS_TAKE_BACK_TEXT);
  for (const std::wstring& slot : slots) content += L"\n" + slot;
  int pressed = IDNO;
  // The safe answer is the default one.
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
  config.pszWindowTitle = kProductName;
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszMainInstruction = title.c_str();
  config.pszContent = content.c_str();
  config.nDefaultButton = IDNO;
  return SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) && pressed == IDYES;
}

}  // namespace pdft
