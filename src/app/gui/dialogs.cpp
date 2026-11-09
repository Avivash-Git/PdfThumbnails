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

SaveAnswer AskSaveChanges(HWND owner) {
  const std::wstring title = Text(IDS_ASK_SAVE_TITLE);
  const std::wstring save = Text(IDS_ASK_SAVE_SAVE);
  const std::wstring dontSave = Text(IDS_ASK_SAVE_DONT);
  const TASKDIALOG_BUTTON buttons[] = {{IDYES, save.c_str()}, {IDNO, dontSave.c_str()}};
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  config.pszWindowTitle = kProductName;
  config.pszMainInstruction = title.c_str();
  config.cButtons = ARRAYSIZE(buttons);
  config.pButtons = buttons;
  config.nDefaultButton = IDYES;
  int pressed = IDCANCEL;
  if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) return SaveAnswer::kCancel;
  return pressed == IDYES  ? SaveAnswer::kSave
         : pressed == IDNO ? SaveAnswer::kDontSave
                           : SaveAnswer::kCancel;
}

bool ConfirmStopRefresh(HWND owner) {
  const std::wstring title = Text(IDS_ASK_STOP_TITLE);
  const std::wstring content = Text(IDS_ASK_STOP_TEXT);
  const std::wstring stop = Text(IDS_ASK_STOP_STOP);
  const std::wstring keep = Text(IDS_ASK_STOP_KEEP);
  const TASKDIALOG_BUTTON buttons[] = {{IDYES, stop.c_str()}, {IDNO, keep.c_str()}};
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.pszWindowTitle = kProductName;
  config.pszMainInstruction = title.c_str();
  config.pszContent = content.c_str();
  config.cButtons = ARRAYSIZE(buttons);
  config.pButtons = buttons;
  config.nDefaultButton = IDNO;
  int pressed = IDNO;
  return SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) && pressed == IDYES;
}

bool ConfirmRebuild(HWND owner, const std::wstring& question, bool* dontAskAgain) {
  const std::wstring rebuild = Text(IDS_REBUILD_NOW);
  const std::wstring verification = Text(IDS_DONT_ASK_AGAIN);
  const TASKDIALOG_BUTTON buttons[] = {{IDYES, rebuild.c_str()}};
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  config.pszWindowTitle = kProductName;
  config.pszContent = question.c_str();
  config.cButtons = ARRAYSIZE(buttons);
  config.pButtons = buttons;
  config.nDefaultButton = IDCANCEL;
  config.pszVerificationText = verification.c_str();
  int pressed = IDCANCEL;
  BOOL ticked = FALSE;
  const bool yes =
      SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, &ticked)) && pressed == IDYES;
  *dontAskAgain = yes && ticked;
  return yes;
}

bool ConfirmUninstall(HWND owner, const std::wstring& question, bool* clearCache) {
  const std::wstring uninstall = Text(IDS_UNINSTALL_NOW);
  const std::wstring verification = Text(IDS_ALSO_CLEAR_CACHE);
  const TASKDIALOG_BUTTON buttons[] = {{IDYES, uninstall.c_str()}};
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_VERIFICATION_FLAG_CHECKED;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  config.pszWindowTitle = kProductName;
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszMainInstruction = question.c_str();
  config.cButtons = ARRAYSIZE(buttons);
  config.pButtons = buttons;
  config.nDefaultButton = IDCANCEL;
  config.pszVerificationText = verification.c_str();
  int pressed = IDCANCEL;
  BOOL ticked = FALSE;
  const bool yes =
      SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, &ticked)) && pressed == IDYES;
  *clearCache = ticked != FALSE;
  return yes;
}

void ShowChildResult(HWND owner, ResultIcon icon, const std::wstring& title,
                     const std::wstring& text) {
  (void)TaskDialog(owner, nullptr, kProductName, title.c_str(), text.c_str(), TDCBF_CLOSE_BUTTON,
                   icon == ResultIcon::kError     ? TD_ERROR_ICON
                   : icon == ResultIcon::kWarning ? TD_WARNING_ICON
                                                  : TD_INFORMATION_ICON,
                   nullptr);
}

void ShowProblem(HWND owner, const std::wstring& text) {
  (void)TaskDialog(owner, nullptr, kProductName, text.c_str(), nullptr, TDCBF_CLOSE_BUTTON,
                   TD_ERROR_ICON, nullptr);
}

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
