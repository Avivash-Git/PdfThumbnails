#include "main_window.h"

#include <commctrl.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli_args.h"
#include "commands.h"
#include "crop_math.h"
#include "dialogs.h"
#include "instance.h"
#include "log.h"
#include "preview_protocol.h"
#include "preview_renderer.h"
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
constexpr UINT_PTR kBusyTimer = 3;
// While another process holds the operation lock; one that crashed tells nobody it is gone.
constexpr UINT kBusyPollMs = 1000;
// The window shows at the first reading, or after this long saying that it is reading.
constexpr UINT kShowAfterMs = 1500;
// Several commands that end close together cause one reading.
constexpr UINT kStateChangedDelayMs = 500;

constexpr std::array<int, 2> kPreviewTiles = {IDC_PREVIEW_DOC, IDC_PREVIEW_SLIDE};
// The page shapes of the samples, for the drawing that stands in when no preview can be made.
constexpr std::array<std::array<double, 2>, 2> kSamplePagePoints = {{{595, 842}, {960, 540}}};

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
  void SetEnabled(int id, bool enabled) noexcept;
  void ShowZoom(int zoom) noexcept;
  void ZoomChanged(int zoom);
  void OnSlider();
  void OnZoomBoxChanged();
  void OnZoomBoxLeft();
  std::wstring ZoomBoxText() const;
  void RequestPreview();
  void OnPreviewReady();
  void DrawTile(const DRAWITEMSTRUCT& item, size_t index) const noexcept;
  void ShowOnce() noexcept;
  void OnJobDone();
  void ApplyReading(const StateReading& reading);
  void ApplyState();
  void RequestReading();
  void RequestClose();
  void StartCheck(std::optional<RepairMode> fix, WindowActivity activity);
  void OnFixLink();
  bool LockIsTaken() const noexcept;
  void EnterBusyIfLocked();
  void OnBusyTimer();
  void Relaunch() noexcept;
  void OnDrawItem(const DRAWITEMSTRUCT& item) noexcept;
  void PaintBackground(HDC dc) const noexcept;
  INT_PTR ColorStatic(HDC dc, HWND control) const noexcept;

  WindowStart start_;
  HWND dialog_ = nullptr;
  std::optional<WindowJobs> jobs_;
  std::optional<PreviewRenderer> preview_;
  WindowTheme theme_;
  UINT stateChanged_ = 0;
  WindowState state_;
  ProcessContext context_;
  StatusPanel panel_;
  StatusPanel panelBeforeCheck_;
  std::wstring message_;  // what the last check or fix ended with
  WindowEdits saved_;
  WindowEdits edits_;
  bool updatingZoom_ = false;  // the window itself is setting the zoom controls
  std::optional<std::pair<int, int>> previewAsked_;  // zoom and edge
  struct Tile {
    wil::unique_hbitmap bitmap;
    int width = 0;
    int height = 0;
  };
  std::array<Tile, kPreviewTiles.size()> tiles_;
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
      if (w == kBusyTimer) OnBusyTimer();
      return TRUE;
    case WM_COMMAND:
      if (LOWORD(w) == IDCANCEL) {
        RequestClose();
        return TRUE;
      }
      if (LOWORD(w) == IDC_CHECK_NOW) {
        if (DecideControls(state_).checkNowAndFix) {
          StartCheck(std::nullopt, WindowActivity::kChecking);
        }
        return TRUE;
      }
      if (LOWORD(w) == IDC_ZOOM_EDIT && HIWORD(w) == EN_CHANGE) {
        OnZoomBoxChanged();
        return TRUE;
      }
      if (LOWORD(w) == IDC_ZOOM_EDIT && HIWORD(w) == EN_KILLFOCUS) {
        OnZoomBoxLeft();
        return TRUE;
      }
      return FALSE;
    case WM_HSCROLL:
      if (reinterpret_cast<HWND>(l) == Item(IDC_ZOOM_SLIDER)) {
        OnSlider();
        return TRUE;
      }
      return FALSE;
    case kPreviewReadyMessage:
      OnPreviewReady();
      return TRUE;
    case WM_CLOSE:
      RequestClose();
      return TRUE;
    case WM_DESTROY:
      preview_.reset();
      PostQuitMessage(0);
      return TRUE;
    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<const NMHDR*>(l);
      if (header->idFrom == IDC_STATUS_FIX &&
          (header->code == NM_CLICK || header->code == NM_RETURN)) {
        OnFixLink();
        return TRUE;
      }
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
      RequestPreview();
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
  preview_.emplace(dialog_);
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

void MainWindow::SetEnabled(int id, bool enabled) noexcept {
  const HWND control = Item(id);
  // A control that goes off must not keep the keyboard focus.
  if (!enabled && GetFocus() == control) SendMessageW(dialog_, WM_NEXTDLGCTL, 0, FALSE);
  EnableWindow(control, enabled);
}

void MainWindow::ShowZoom(int zoom) noexcept {
  updatingZoom_ = true;
  SendDlgItemMessageW(dialog_, IDC_ZOOM_SLIDER, TBM_SETPOS, TRUE, zoom);
  SetDlgItemInt(dialog_, IDC_ZOOM_EDIT, static_cast<UINT>(zoom), FALSE);
  updatingZoom_ = false;
}

void MainWindow::ZoomChanged(int zoom) {
  if (edits_.zoom == zoom) return;
  edits_.zoom = zoom;
  state_.dirty = edits_ != saved_;
  RequestPreview();
}

void MainWindow::OnSlider() {
  const int zoom =
      static_cast<int>(SendDlgItemMessageW(dialog_, IDC_ZOOM_SLIDER, TBM_GETPOS, 0, 0));
  updatingZoom_ = true;
  SetDlgItemInt(dialog_, IDC_ZOOM_EDIT, static_cast<UINT>(zoom), FALSE);
  updatingZoom_ = false;
  ZoomChanged(zoom);
}

std::wstring MainWindow::ZoomBoxText() const {
  wchar_t text[16] = {};
  const UINT length = GetDlgItemTextW(dialog_, IDC_ZOOM_EDIT, text, ARRAYSIZE(text));
  return std::wstring(text, length);
}

void MainWindow::OnZoomBoxChanged() {
  if (updatingZoom_) return;
  // While someone types, only a whole valid number counts; leaving the box settles the rest.
  const std::optional<int> zoom = ParseZoomValue(ZoomBoxText());
  if (!zoom) return;
  updatingZoom_ = true;
  SendDlgItemMessageW(dialog_, IDC_ZOOM_SLIDER, TBM_SETPOS, TRUE, *zoom);
  updatingZoom_ = false;
  ZoomChanged(*zoom);
}

void MainWindow::OnZoomBoxLeft() {
  const int zoom = ZoomFromBox(ZoomBoxText(), edits_.zoom);
  ShowZoom(zoom);
  ZoomChanged(zoom);
}

void MainWindow::RequestPreview() {
  if (state_.context == WindowContext::kStarting || state_.context == WindowContext::kSandboxed) {
    return;
  }
  const std::pair<int, int> asked{edits_.zoom, PreviewEdgeForDpi(GetDpiForWindow(dialog_))};
  if (previewAsked_ == asked) return;
  previewAsked_ = asked;
  preview_->Request(asked.first, asked.second);
}

void MainWindow::OnPreviewReady() {
  std::optional<PreviewResult> result = preview_->Take();
  if (!result) return;
  for (size_t i = 0; i < tiles_.size(); ++i) {
    tiles_[i] = Tile{};
    if (result->ok && result->frames[i].hr == S_OK) {
      const PreviewFrame& frame = result->frames[i];
      BITMAPINFO format = {};
      format.bmiHeader.biSize = sizeof(format.bmiHeader);
      format.bmiHeader.biWidth = static_cast<LONG>(frame.width);
      format.bmiHeader.biHeight = -static_cast<LONG>(frame.height);
      format.bmiHeader.biPlanes = 1;
      format.bmiHeader.biBitCount = 32;
      format.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      wil::unique_hbitmap bitmap(
          CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &bits, nullptr, 0));
      if (bitmap && bits) {
        memcpy(bits, frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
        tiles_[i].bitmap = std::move(bitmap);
        tiles_[i].width = static_cast<int>(frame.width);
        tiles_[i].height = static_cast<int>(frame.height);
      }
    }
    InvalidateRect(Item(kPreviewTiles[i]), nullptr, FALSE);
  }
}

void MainWindow::DrawTile(const DRAWITEMSTRUCT& item, size_t index) const noexcept {
  FillRect(item.hDC, &item.rcItem, theme_.content());
  const int areaW = item.rcItem.right - item.rcItem.left;
  const int areaH = item.rcItem.bottom - item.rcItem.top;
  const Tile& tile = tiles_[index];
  if (tile.bitmap) {
    wil::unique_hdc memory(CreateCompatibleDC(item.hDC));
    if (memory) {
      const auto selected = wil::SelectObject(memory.get(), tile.bitmap.get());
      BitBlt(item.hDC, item.rcItem.left + (areaW - tile.width) / 2,
             item.rcItem.top + (areaH - tile.height) / 2, tile.width, tile.height, memory.get(), 0,
             0, SRCCOPY);
    }
  } else if (state_.context != WindowContext::kStarting) {
    // No preview: the shape a thumbnail of this zoom has.
    const CropGeometry shape = ComputeCrop(
        kSamplePagePoints[index][0], kSamplePagePoints[index][1],
        static_cast<unsigned>(PreviewEdgeForDpi(GetDpiForWindow(dialog_))), edits_.zoom);
    RECT page = {item.rcItem.left + (areaW - shape.bmpW) / 2,
                 item.rcItem.top + (areaH - shape.bmpH) / 2, 0, 0};
    page.right = page.left + shape.bmpW;
    page.bottom = page.top + shape.bmpH;
    FillRect(item.hDC, &page, GetSysColorBrush(COLOR_BTNFACE));
    FrameRect(item.hDC, &page, GetSysColorBrush(COLOR_GRAYTEXT));
  }
  FrameRect(item.hDC, &item.rcItem, GetSysColorBrush(COLOR_GRAYTEXT));
}

void MainWindow::ShowOnce() noexcept {
  if (shown_) return;
  shown_ = true;
  KillTimer(dialog_, kShowTimer);
  ShowWindow(dialog_, SW_SHOWNORMAL);
  SetForegroundWindow(dialog_);
}

void MainWindow::StartCheck(std::optional<RepairMode> fix, WindowActivity activity) {
  panelBeforeCheck_ = panel_;
  panel_.lines[0] = {StatusGlyph::kNone, Text(IDS_CHECKING)};
  message_.clear();
  state_.activity = activity;
  ApplyState();
  jobs_->StartCheck(context_, dialog_, fix);
}

void MainWindow::OnFixLink() {
  if (!DecideControls(state_).checkNowAndFix || panel_.fix == FixLink::kNone) return;
  if (panel_.fix == FixLink::kTakeBack && !ConfirmTakeBack(dialog_, panel_.takeBack)) return;
  StartCheck(panel_.fix == FixLink::kTakeBack ? RepairMode::kSafeAndTakeBack : RepairMode::kSafe,
             WindowActivity::kFixing);
}

bool MainWindow::LockIsTaken() const noexcept {
  std::optional<OperationLock> lock;
  const HRESULT hr = OperationLock::Acquire(0, &lock);
  return hr == OperationLock::kBusy || hr == OperationLock::kTakenByLowerIntegrity;
}

void MainWindow::EnterBusyIfLocked() {
  if (state_.context != WindowContext::kReady || state_.activity != WindowActivity::kIdle ||
      !LockIsTaken()) {
    return;
  }
  state_.activity = WindowActivity::kExternalBusy;
  SetTimer(dialog_, kBusyTimer, kBusyPollMs, nullptr);
  ApplyState();
}

void MainWindow::OnBusyTimer() {
  if (state_.activity != WindowActivity::kExternalBusy) return;
  if (LockIsTaken()) {
    SetTimer(dialog_, kBusyTimer, kBusyPollMs, nullptr);
    return;
  }
  state_.activity = WindowActivity::kIdle;
  ApplyState();
  RequestReading();
}

void MainWindow::OnJobDone() {
  std::optional<StateReading> reading = jobs_->TakeReading();
  state_.activity = WindowActivity::kIdle;
  if (reading && reading->check) {
    panel_ = panelBeforeCheck_;
    message_ = CheckOutcomeText(*reading->check);
    if (reading->check->busy) {
      state_.activity = WindowActivity::kExternalBusy;
      SetTimer(dialog_, kBusyTimer, kBusyPollMs, nullptr);
    }
  }
  if (reading && reading->leaveContainer) {
    Relaunch();
    return;
  }
  if (reading) ApplyReading(*reading);
  ApplyState();
  RequestPreview();
  if (!closePending_) EnterBusyIfLocked();
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
  if (!reading.snapshotRead) return;
  state_.context = ClassifyWindowContext(RefusesWrites(context_.virtualization),
                                         reading.snapshot.scope != Scope::kNone);
  panel_ = BuildStatusPanel(reading.snapshot, reading.findings);
  // Someone's unsaved edits stay; only what they are compared with moves.
  saved_ = {reading.snapshot.settings.zoom, reading.snapshot.task.afterPdfCraft,
            reading.snapshot.task.atSignIn};
  if (state_.dirty) {
    state_.dirty = edits_ != saved_;
    return;
  }
  edits_ = saved_;
  ShowZoom(edits_.zoom);
  CheckDlgButton(dialog_, IDC_CHECK_PDFCRAFT,
                 reading.snapshot.task.afterPdfCraft ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(dialog_, IDC_CHECK_SIGNIN,
                 reading.snapshot.task.atSignIn ? BST_CHECKED : BST_UNCHECKED);
}

void MainWindow::ApplyState() {
  const ControlStates controls = DecideControls(state_);
  for (const int id : {IDC_ZOOM_SLIDER, IDC_ZOOM_EDIT, IDC_ZOOM_SPIN}) {
    SetEnabled(id, controls.zoomAndChecks);
  }
  SetEnabled(IDC_CHECK_NOW, controls.checkNowAndFix);

  for (size_t i = 0; i < panel_.lines.size(); ++i) {
    SetText(kLineControls[i], panel_.lines[i].text);
    InvalidateRect(Item(kGlyphControls[i]), nullptr, TRUE);
  }
  const bool showFix = panel_.fix != FixLink::kNone && controls.checkNowAndFix;
  if (showFix) {
    SetText(IDC_STATUS_FIX, panel_.fix == FixLink::kFix ? Text(IDS_FIX)
                            : panel_.fix == FixLink::kFixNeedsAdmin
                                ? Text(IDS_FIX_ADMIN)
                                : Filled(Text(IDS_FIX_TAKE_BACK), L"{product}", kProductName));
  }
  SetVisible(IDC_STATUS_FIX, showFix);
  // One line for both: what to do about the problem comes before how the last check ended.
  const std::wstring& note = panel_.hint.empty() ? message_ : panel_.hint;
  SetText(IDC_STATUS_HINT, note);
  SetVisible(IDC_STATUS_HINT, !showFix && !note.empty());

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
    case CloseAction::kAskSaveChanges:
      // Nothing can be saved yet: an unsaved zoom is simply left.
      if (!jobs_->running()) {
        DestroyWindow(dialog_);
        break;
      }
      [[fallthrough]];
    case CloseAction::kWaitForWork:
    case CloseAction::kAskStopRefresh:
      closePending_ = true;
      SetText(IDC_STATUS_LINE1, Text(IDS_CLOSING_AFTER_WORK));
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
  for (size_t i = 0; i < kPreviewTiles.size(); ++i) {
    if (static_cast<int>(item.CtlID) == kPreviewTiles[i]) DrawTile(item, i);
  }
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
