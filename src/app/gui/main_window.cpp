#include "main_window.h"

#include <commctrl.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "commands.h"
#include "instance.h"
#include "log.h"
#include "project_config.h"
#include "resource_ids.h"
#include "status_panel.h"
#include "strings.h"
#include "theme.h"
#include "window_jobs.h"
#include "window_messages.h"
#include "window_model.h"

namespace pdft {
namespace {

constexpr UINT_PTR kShowTimer = 1;
constexpr UINT_PTR kStateChangedTimer = 2;
// The window shows at the first reading, or after this long saying that it is reading.
constexpr UINT kShowAfterMs = 1500;
// Several commands that end close together cause one reading.
constexpr UINT kStateChangedDelayMs = 500;

constexpr std::array<int, 3> kHeadings = {IDC_HEAD_ZOOM, IDC_HEAD_CHECKS, IDC_HEAD_STATUS};
constexpr std::array<int, 3> kGlyphControls = {IDC_STATUS_GLYPH1, IDC_STATUS_GLYPH2,
                                               IDC_STATUS_GLYPH3};
constexpr std::array<int, 3> kLineControls = {IDC_STATUS_LINE1, IDC_STATUS_LINE2, IDC_STATUS_LINE3};

// A string from this executable's string table.
std::wstring Text(UINT id) {
  const wchar_t* text = nullptr;
  const int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
  return length > 0 && text ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
}

std::wstring Filled(std::wstring text, std::wstring_view name, std::wstring_view value) {
  if (const size_t at = text.find(name); at != std::wstring::npos) {
    text.replace(at, name.size(), value);
  }
  return text;
}

class MainWindow {
 public:
  explicit MainWindow(const WindowStart& start) : start_(start) {}

  WindowExit Run();

 private:
  static INT_PTR CALLBACK Procedure(HWND dialog, UINT message, WPARAM w, LPARAM l);
  INT_PTR Handle(UINT message, WPARAM w, LPARAM l);

  HWND Item(int id) const noexcept { return GetDlgItem(dialog_, id); }
  void SetVisible(int id, bool visible) noexcept {
    ShowWindow(Item(id), visible ? SW_SHOWNA : SW_HIDE);
  }
  void SetText(int id, const std::wstring& text) noexcept {
    SetDlgItemTextW(dialog_, id, text.c_str());
  }

  void OnInitDialog();
  void ApplyFonts() noexcept;
  void SetUpZoomControls() noexcept;
  void ShowOnce() noexcept;
  void OnJobDone();
  void ApplyReading(const StateReading& reading);
  void ApplyState();
  void RequestReading();
  void RequestClose();
  void Relaunch() noexcept;
  void OnDrawItem(const DRAWITEMSTRUCT& item) noexcept;
  void PaintBackground(HDC dc) const noexcept;
  INT_PTR ColorStatic(HDC dc, HWND control) const noexcept;

  WindowStart start_;
  HWND dialog_ = nullptr;
  std::optional<WindowJobs> jobs_;
  WindowTheme theme_;
  UINT stateChanged_ = 0;
  WindowState state_;
  ProcessContext context_;
  StatusPanel panel_;
  bool shown_ = false;
  bool closePending_ = false;
  bool readAgain_ = false;
  WindowExit exit_;
};

INT_PTR CALLBACK MainWindow::Procedure(HWND dialog, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_INITDIALOG) {
    SetWindowLongPtrW(dialog, DWLP_USER, l);
    auto* window = reinterpret_cast<MainWindow*>(l);
    window->dialog_ = dialog;
  }
  auto* window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
  return window ? window->Handle(message, w, l) : FALSE;
}

INT_PTR MainWindow::Handle(UINT message, WPARAM w, LPARAM l) try {
  if (stateChanged_ != 0 && message == stateChanged_) {
    SetTimer(dialog_, kStateChangedTimer, kStateChangedDelayMs, nullptr);
    return TRUE;
  }
  switch (message) {
    case WM_INITDIALOG:
      OnInitDialog();
      return FALSE;  // the focus stays where the dialog manager put it
    case kActivateWindowMessage:
      if (IsIconic(dialog_)) ShowWindow(dialog_, SW_RESTORE);
      SetForegroundWindow(dialog_);
      return TRUE;
    case kJobDoneMessage:
      OnJobDone();
      return TRUE;
    case WM_TIMER:
      KillTimer(dialog_, w);
      if (w == kShowTimer) ShowOnce();
      if (w == kStateChangedTimer) RequestReading();
      return TRUE;
    case WM_COMMAND:
      if (LOWORD(w) == IDCANCEL) {
        RequestClose();
        return TRUE;
      }
      return FALSE;
    case WM_CLOSE:
      RequestClose();
      return TRUE;
    case WM_DESTROY:
      PostQuitMessage(0);
      return TRUE;
    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<const NMHDR*>(l);
      if (header->idFrom == IDC_BANNER_LINK &&
          (header->code == NM_CLICK || header->code == NM_RETURN)) {
        Relaunch();
        return TRUE;
      }
      return FALSE;
    }
    case WM_DRAWITEM:
      OnDrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(l));
      return TRUE;
    case WM_ERASEBKGND:
      PaintBackground(reinterpret_cast<HDC>(w));
      SetWindowLongPtrW(dialog_, DWLP_MSGRESULT, 1);
      return TRUE;
    case WM_CTLCOLORSTATIC:
      return ColorStatic(reinterpret_cast<HDC>(w), reinterpret_cast<HWND>(l));
    case WM_CTLCOLORBTN: {
      const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
      return reinterpret_cast<INT_PTR>(
          id == IDOK || id == IDCANCEL || id == IDC_UNINSTALL ? theme_.footer() : theme_.content());
    }
    case WM_DPICHANGED:
      theme_.Rebuild(HIWORD(w));
      ApplyFonts();
      return FALSE;  // the dialog manager scales the window and its controls
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      theme_.Rebuild(GetDpiForWindow(dialog_));
      InvalidateRect(dialog_, nullptr, TRUE);
      return FALSE;
    default:
      return FALSE;
  }
} catch (...) {
  return FALSE;  // out of memory: this message is dropped, the window goes on
}

void MainWindow::OnInitDialog() {
  SetWindowTextW(dialog_, (std::wstring(kProductName) + L" Settings").c_str());
  theme_.Rebuild(GetDpiForWindow(dialog_));
  for (const int id : kHeadings) {
    SetDialogControlDpiChangeBehavior(Item(id), DCDC_DISABLE_FONT_UPDATE, DCDC_DISABLE_FONT_UPDATE);
  }
  ApplyFonts();
  SetUpZoomControls();
  stateChanged_ = StateChangedMessage();
  SetText(IDC_STATUS_LINE1, Text(IDS_READING_STATE));
  jobs_.emplace(dialog_);
  jobs_->StartFirstReading(start_.relaunched);
  state_.activity = WindowActivity::kGathering;
  SetTimer(dialog_, kShowTimer, kShowAfterMs, nullptr);
}

void MainWindow::ApplyFonts() noexcept {
  for (const int id : kHeadings) {
    SendMessageW(Item(id), WM_SETFONT, reinterpret_cast<WPARAM>(theme_.heading()), TRUE);
  }
}

void MainWindow::SetUpZoomControls() noexcept {
  const HWND slider = Item(IDC_ZOOM_SLIDER);
  SendMessageW(slider, TBM_SETRANGEMIN, FALSE, 100);
  SendMessageW(slider, TBM_SETRANGEMAX, TRUE, 200);
  SendMessageW(slider, TBM_SETLINESIZE, 0, 1);
  SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
  for (const int tick : kZoomTicks) SendMessageW(slider, TBM_SETTIC, 0, tick);
  SendDlgItemMessageW(dialog_, IDC_ZOOM_EDIT, EM_SETLIMITTEXT, 3, 0);
  SendDlgItemMessageW(dialog_, IDC_ZOOM_SPIN, UDM_SETRANGE32, 100, 200);
}

void MainWindow::ShowOnce() noexcept {
  if (shown_) return;
  shown_ = true;
  KillTimer(dialog_, kShowTimer);
  ShowWindow(dialog_, SW_SHOWNORMAL);
  SetForegroundWindow(dialog_);
}

void MainWindow::OnJobDone() {
  std::optional<StateReading> reading = jobs_->TakeReading();
  state_.activity = WindowActivity::kIdle;
  if (reading && reading->leaveContainer) {
    Relaunch();
    return;
  }
  if (reading) ApplyReading(*reading);
  ApplyState();
  ShowOnce();
  if (closePending_) {
    DestroyWindow(dialog_);
    return;
  }
  if (readAgain_) {
    readAgain_ = false;
    RequestReading();
  }
}

void MainWindow::ApplyReading(const StateReading& reading) {
  if (reading.contextRead) {
    context_ = reading.context;
    state_.otherUser = context_.differentFromShellUser;
  }
  if (FAILED(reading.hr)) {
    panel_ = StatusPanel{};
    panel_.lines[0] = {StatusGlyph::kProblem,
                       Filled(Text(IDS_STATE_UNREADABLE), L"{hr}", HResultText(reading.hr))};
    if (reading.contextRead && RefusesWrites(context_.virtualization)) {
      state_.context = WindowContext::kSandboxed;
    }
    return;
  }
  state_.context = ClassifyWindowContext(RefusesWrites(context_.virtualization),
                                         reading.snapshot.scope != Scope::kNone);
  panel_ = BuildStatusPanel(reading.snapshot, reading.findings);
  SendDlgItemMessageW(dialog_, IDC_ZOOM_SLIDER, TBM_SETPOS, TRUE, reading.snapshot.settings.zoom);
  SetDlgItemInt(dialog_, IDC_ZOOM_EDIT, static_cast<UINT>(reading.snapshot.settings.zoom), FALSE);
  CheckDlgButton(dialog_, IDC_CHECK_PDFCRAFT,
                 reading.snapshot.task.afterPdfCraft ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(dialog_, IDC_CHECK_SIGNIN,
                 reading.snapshot.task.atSignIn ? BST_CHECKED : BST_UNCHECKED);
}

void MainWindow::ApplyState() {
  for (size_t i = 0; i < panel_.lines.size(); ++i) {
    SetText(kLineControls[i], panel_.lines[i].text);
    InvalidateRect(Item(kGlyphControls[i]), nullptr, TRUE);
  }
  SetText(IDC_STATUS_HINT, panel_.hint);
  SetVisible(IDC_STATUS_HINT, !panel_.hint.empty());

  const WindowBanner banner = DecideBanner(state_);
  std::wstring text;
  switch (banner) {
    case WindowBanner::kNone:
      break;
    case WindowBanner::kSandboxed:
      text = context_.packagedAncestor
                 ? Filled(Text(IDS_BANNER_SANDBOXED), L"{product}", kProductName)
                 : CannotRunHereMessage(context_);
      break;
    case WindowBanner::kOtherUser:
      text = Filled(Text(IDS_BANNER_OTHER_USER), L"{account}", context_.userAccount);
      break;
    case WindowBanner::kExternalBusy:
      text = Text(IDS_BANNER_BUSY);
      break;
    case WindowBanner::kNotInstalled:
      text = Text(IDS_BANNER_NOT_INSTALLED);
      break;
  }
  SetText(IDC_BANNER, text);
  SetVisible(IDC_BANNER, banner != WindowBanner::kNone);
  SetVisible(IDC_BANNER_LINK, banner == WindowBanner::kOtherUser);
}

void MainWindow::RequestReading() {
  if (state_.context == WindowContext::kStarting) return;
  if (jobs_->running() || (state_.activity != WindowActivity::kIdle &&
                           state_.activity != WindowActivity::kExternalBusy)) {
    readAgain_ = true;
    return;
  }
  state_.activity = WindowActivity::kGathering;
  jobs_->StartReading(context_);
}

void MainWindow::RequestClose() {
  switch (DecideClose(state_)) {
    case CloseAction::kCloseNow:
      DestroyWindow(dialog_);
      break;
    case CloseAction::kWaitForWork:
    case CloseAction::kAskSaveChanges:
    case CloseAction::kAskStopRefresh:
      closePending_ = true;
      break;
  }
}

void MainWindow::Relaunch() noexcept {
  exit_.relaunch = true;
  DestroyWindow(dialog_);
}

void MainWindow::OnDrawItem(const DRAWITEMSTRUCT& item) noexcept {
  for (size_t i = 0; i < kGlyphControls.size(); ++i) {
    if (static_cast<int>(item.CtlID) == kGlyphControls[i]) {
      theme_.DrawGlyph(item.hDC, item.rcItem, panel_.lines[i].glyph);
      return;
    }
  }
  // A preview tile: an empty frame until it has something to show.
  FillRect(item.hDC, &item.rcItem, theme_.content());
  FrameRect(item.hDC, &item.rcItem, GetSysColorBrush(COLOR_GRAYTEXT));
}

void MainWindow::PaintBackground(HDC dc) const noexcept {
  RECT client = {};
  GetClientRect(dialog_, &client);
  RECT band = {0, kFooterTopDlu, 0, 0};
  MapDialogRect(dialog_, &band);
  RECT content = client;
  content.bottom = band.top;
  FillRect(dc, &content, theme_.content());
  RECT footer = client;
  footer.top = band.top;
  FillRect(dc, &footer, theme_.footer());
  RECT separator = footer;
  separator.bottom = separator.top + 1;
  FillRect(dc, &separator, GetSysColorBrush(COLOR_3DSHADOW));
}

INT_PTR MainWindow::ColorStatic(HDC dc, HWND control) const noexcept {
  SetBkMode(dc, TRANSPARENT);
  SetBkColor(dc, GetSysColor(COLOR_WINDOW));
  const int id = GetDlgCtrlID(control);
  SetTextColor(dc, GetSysColor(id == IDC_ZOOM_HINT ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
  return reinterpret_cast<INT_PTR>(theme_.content());
}

WindowExit MainWindow::Run() {
  INITCOMMONCONTROLSEX classes = {sizeof(classes), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
                                                       ICC_UPDOWN_CLASS | ICC_PROGRESS_CLASS |
                                                       ICC_LINK_CLASS};
  if (!InitCommonControlsEx(&classes)) return {static_cast<int>(ExitCode::kFailed), false};
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const std::wstring className = SettingsWindowClassName();
  WNDCLASSEXW windowClass = {sizeof(windowClass)};
  windowClass.lpfnWndProc = DefDlgProcW;
  windowClass.cbWndExtra = DLGWINDOWEXTRA;
  windowClass.hInstance = instance;
  windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = className.c_str();
  if (!RegisterClassExW(&windowClass)) return {static_cast<int>(ExitCode::kFailed), false};

  if (!CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, &Procedure,
                          reinterpret_cast<LPARAM>(this))) {
    Log(LogLevel::kWarning, L"window",
        L"window not created hr=" + HResultText(HRESULT_FROM_WIN32(GetLastError())));
    return {static_cast<int>(ExitCode::kFailed), false};
  }
  const HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDA_SETTINGS));
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (TranslateAcceleratorW(dialog_, accelerators, &message)) continue;
    if (IsDialogMessageW(dialog_, &message)) continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return exit_;
}

}  // namespace

WindowExit OpenMainWindow(const WindowStart& start) noexcept try {
  MainWindow window(start);
  return window.Run();
} catch (...) {
  return {static_cast<int>(ExitCode::kFailed), false};
}

}  // namespace pdft
