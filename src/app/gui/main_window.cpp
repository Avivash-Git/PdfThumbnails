#include "main_window.h"

#include <commctrl.h>
#include <shellapi.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "accessibility.h"
#include "child_operation.h"
#include "cli_args.h"
#include "commands.h"
#include "crop_math.h"
#include "diagnostics.h"
#include "dialogs.h"
#include "instance.h"
#include "log.h"
#include "paths.h"
#include "preview_protocol.h"
#include "preview_renderer.h"
#include "project_config.h"
#include "rebuild.h"
#include "registry_roots.h"
#include "resource_ids.h"
#include "settings.h"
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

// Counts a question being open: its own message loop keeps delivering the window's messages.
class ModalScope {
 public:
  explicit ModalScope(int* count) noexcept : count_(count) { ++*count_; }
  ~ModalScope() { --*count_; }
  ModalScope(const ModalScope&) = delete;
  ModalScope& operator=(const ModalScope&) = delete;

 private:
  int* count_;
};

class MainWindow {
 public:
  explicit MainWindow(const WindowStart& start) : start_(start) {}

  WindowExit Run();

 private:
  static INT_PTR CALLBACK Procedure(HWND dialog, UINT message, WPARAM w, LPARAM l);
  INT_PTR Handle(UINT message, WPARAM w, LPARAM l);

  HWND Item(int id) const noexcept { return GetDlgItem(dialog_, id); }
  void SetVisible(int id, bool visible) noexcept {
    // A control that goes away must not keep the keyboard focus.
    if (!visible && GetFocus() == Item(id)) SendMessageW(dialog_, WM_NEXTDLGCTL, 0, FALSE);
    ShowWindow(Item(id), visible ? SW_SHOWNA : SW_HIDE);
  }
  void SetText(int id, const std::wstring& text) noexcept {
    SetDlgItemTextW(dialog_, id, text.c_str());
  }

  void OnInitDialog();
  void ApplyFonts() noexcept;
  void DescribeControls();
  void DescribeTiles();
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
  void OnRebuild();
  void OnUninstall();
  void OnInstall(bool forAllUsers);
  void StartChild(std::wstring arguments, UINT titleId, bool closeOnSuccess);
  void OnChildLine();
  void OnChildDone();
  void OnSave();
  void OnCancelRefresh();
  void OnRefreshProgress();
  void OnCheckBoxChanged();
  void OnOpenLog();
  void OnCopyDiagnostics();
  void OnDiagnosticsDone();
  void ShowNote(std::wstring note);
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
  // The report may be asked for while a check runs, so it has a thread of its own.
  std::optional<WindowJobs> diagnosticsJob_;
  std::optional<PreviewRenderer> preview_;
  WindowTheme theme_;
  WindowAccessibility accessibility_;
  std::wstring announcedLine_;  // what screen readers were told last ...
  std::wstring announcedBanner_;
  ULONGLONG announcedAt_ = 0;  // ... and when: a running refresh is read out at most once a second
  UINT stateChanged_ = 0;
  WindowState state_;
  ProcessContext context_;
  StatusPanel panel_;
  StatusPanel panelBeforeCheck_;
  std::wstring message_;  // what the last check or fix ended with
  std::optional<ChildOperation> child_;
  UINT childTitle_ = 0;
  bool closeAfterChild_ = false;  // an uninstall that worked leaves nothing for the window to show
  bool confirmFullRefresh_ = true;
  bool canChangeChecks_ = false;    // an installed settings program exists for the task to run
  std::wstring pdfCraftCheckText_;  // the check box's own text
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
  int questionsOpen_ = 0;
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
    case kDiagnosticsDoneMessage:
      OnDiagnosticsDone();
      return TRUE;
    case kRefreshProgressMessage:
      OnRefreshProgress();
      return TRUE;
    case kChildLineMessage:
      OnChildLine();
      return TRUE;
    case kChildDoneMessage:
      OnChildDone();
      return TRUE;
    case WM_ENDSESSION:
      // Windows is ending the session: a refresh stops, and gets a moment to end cleanly.
      if (w) {
        jobs_->RequestStop();
        jobs_->WaitForEnd(3000);
      }
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
      if (LOWORD(w) == IDC_REBUILD && HIWORD(w) == BN_CLICKED) {
        OnRebuild();
        return TRUE;
      }
      if (LOWORD(w) == IDC_UNINSTALL && HIWORD(w) == BN_CLICKED) {
        OnUninstall();
        return TRUE;
      }
      if ((LOWORD(w) == IDC_INSTALL_MACHINE || LOWORD(w) == IDC_INSTALL_USER) &&
          HIWORD(w) == BN_CLICKED) {
        OnInstall(LOWORD(w) == IDC_INSTALL_MACHINE);
        return TRUE;
      }
      if (LOWORD(w) == IDOK) {
        OnSave();
        return TRUE;
      }
      if ((LOWORD(w) == IDC_CHECK_PDFCRAFT || LOWORD(w) == IDC_CHECK_SIGNIN) &&
          HIWORD(w) == BN_CLICKED) {
        OnCheckBoxChanged();
        return TRUE;
      }
      if (LOWORD(w) == IDC_OPEN_LOG && HIWORD(w) == BN_CLICKED) {
        OnOpenLog();
        return TRUE;
      }
      if (LOWORD(w) == IDC_COPY_DIAG && HIWORD(w) == BN_CLICKED) {
        OnCopyDiagnostics();
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
      // A child goes on by itself; the window only stops listening.
      child_.reset();
      PostQuitMessage(0);
      return TRUE;
    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<const NMHDR*>(l);
      if (header->idFrom == IDC_PROGRESS_CANCEL &&
          (header->code == NM_CLICK || header->code == NM_RETURN)) {
        OnCancelRefresh();
        return TRUE;
      }
      if (header->idFrom == IDC_STATUS_FIX &&
          (header->code == NM_CLICK || header->code == NM_RETURN)) {
        OnFixLink();
        return TRUE;
      }
      if (header->idFrom == IDC_BANNER_LINK &&
          (header->code == NM_CLICK || header->code == NM_RETURN)) {
        // Only when closing now would lose nothing.
        if (DecideClose(state_) == CloseAction::kCloseNow) Relaunch();
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
  accessibility_.Start();
  DescribeControls();
  wchar_t checkText[128] = {};
  GetDlgItemTextW(dialog_, IDC_CHECK_PDFCRAFT, checkText, ARRAYSIZE(checkText));
  pdfCraftCheckText_ = checkText;
  stateChanged_ = StateChangedMessage();
  SetText(IDC_STATUS_LINE1, Text(IDS_READING_STATE));
  preview_.emplace(dialog_);
  child_.emplace(dialog_);
  jobs_.emplace(dialog_);
  diagnosticsJob_.emplace(dialog_, kDiagnosticsDoneMessage);
  jobs_->StartFirstReading(start_.relaunched);
  state_.activity = WindowActivity::kGathering;
  SetTimer(dialog_, kShowTimer, kShowAfterMs, nullptr);
}

void MainWindow::DescribeControls() {
  accessibility_.SetName(Item(IDC_ZOOM_SLIDER), Text(IDS_NAME_ZOOM_SLIDER));
  accessibility_.SetName(Item(IDC_ZOOM_EDIT), Text(IDS_NAME_ZOOM_BOX));
  accessibility_.SetName(Item(IDC_ZOOM_SPIN), Text(IDS_NAME_ZOOM_ARROWS));
  accessibility_.SetName(Item(IDC_PROGRESS), Text(IDS_NAME_PROGRESS));
  for (const int id : kGlyphControls) accessibility_.MarkAsPicture(Item(id));
  for (const int id : kPreviewTiles) accessibility_.MarkAsPicture(Item(id));
  DescribeTiles();
  accessibility_.MakeLiveRegion(Item(IDC_STATUS_LINE1), false);
  accessibility_.MakeLiveRegion(Item(IDC_BANNER), true);
}

void MainWindow::DescribeTiles() {
  const std::wstring zoom = std::to_wstring(edits_.zoom);
  accessibility_.SetName(Item(IDC_PREVIEW_DOC), Filled(Text(IDS_NAME_TILE_DOC), L"{zoom}", zoom));
  accessibility_.SetName(Item(IDC_PREVIEW_SLIDE),
                         Filled(Text(IDS_NAME_TILE_SLIDE), L"{zoom}", zoom));
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
  DescribeTiles();
  ApplyState();
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
  // While a child runs there is no preview; the window asks again when it has ended.
  if (!preview_ || state_.context == WindowContext::kStarting ||
      state_.context == WindowContext::kSandboxed) {
    return;
  }
  const std::pair<int, int> asked{edits_.zoom, PreviewEdgeForDpi(GetDpiForWindow(dialog_))};
  if (previewAsked_ == asked) return;
  previewAsked_ = asked;
  preview_->Request(asked.first, asked.second);
}

void MainWindow::OnPreviewReady() {
  if (!preview_) return;  // this message outlived the renderer
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
  const FixLink fix = panel_.fix;
  if (fix == FixLink::kTakeBack) {
    {
      const ModalScope question(&questionsOpen_);
      if (!ConfirmTakeBack(dialog_, panel_.takeBack)) return;
    }
    if (!DecideControls(state_).checkNowAndFix) return;
  }
  StartCheck(fix == FixLink::kTakeBack ? RepairMode::kSafeAndTakeBack : RepairMode::kSafe,
             WindowActivity::kFixing);
}

void MainWindow::OnRebuild() {
  if (!DecideControls(state_).rebuild) return;
  if (confirmFullRefresh_) {
    bool dontAskAgain = false;
    {
      const ModalScope question(&questionsOpen_);
      if (!ConfirmRebuild(dialog_, RebuildQuestion(), &dontAskAgain)) return;
    }
    // Things may have started while the question was open.
    if (!DecideControls(state_).rebuild) return;
    if (dontAskAgain && SUCCEEDED(SaveConfirmFullRefresh(RegistryRoots{}, false))) {
      confirmFullRefresh_ = false;
    }
  }
  StartChild(L"--refresh --full --yes", IDS_CHILD_TITLE_REBUILD, false);
}

void MainWindow::OnUninstall() {
  if (!DecideControls(state_).uninstall) return;
  bool clearCache = true;
  {
    const ModalScope question(&questionsOpen_);
    if (!ConfirmUninstall(dialog_, UninstallQuestion(false), &clearCache)) return;
  }
  if (!DecideControls(state_).uninstall) return;
  StartChild(clearCache ? L"--uninstall --yes --rebuild-thumbnails" : L"--uninstall --yes",
             IDS_CHILD_TITLE_UNINSTALL, true);
}

void MainWindow::OnInstall(bool forAllUsers) {
  if (!kDevInstall || !DecideControls(state_).install) return;
  StartChild(forAllUsers ? L"--install --machine" : L"--install --user", IDS_CHILD_TITLE_INSTALL,
             false);
}

void MainWindow::StartChild(std::wstring arguments, UINT titleId, bool closeOnSuccess) {
  // The preview's child holds the handler, which a rebuild or an uninstall has to be able to
  // replace. The window holds no lock while it is idle.
  preview_.reset();
  previewAsked_.reset();
  childTitle_ = titleId;
  closeAfterChild_ = closeOnSuccess;
  panelBeforeCheck_ = panel_;
  message_.clear();
  state_.activity = WindowActivity::kChild;
  ApplyState();
  child_->Start(std::move(arguments));
}

void MainWindow::OnChildLine() {
  const std::optional<std::wstring> line = child_->TakeLine();
  if (!line || line->empty() || state_.activity != WindowActivity::kChild) return;
  panel_.lines[0] = {StatusGlyph::kNone, *line};
  ApplyState();
}

void MainWindow::OnChildDone() {
  const std::optional<ChildResult> result = child_->TakeResult();
  panel_ = panelBeforeCheck_;
  state_.activity = WindowActivity::kIdle;
  preview_.emplace(dialog_);
  ApplyState();
  if (!result) return;
  const bool started = SUCCEEDED(result->hr);
  const ChildOutcome outcome = started ? OutcomeOfChild(result->exitCode) : ChildOutcome::kError;
  const ModalScope question(&questionsOpen_);
  ShowChildResult(dialog_,
                  outcome == ChildOutcome::kError     ? ResultIcon::kError
                  : outcome == ChildOutcome::kWarning ? ResultIcon::kWarning
                                                      : ResultIcon::kInformation,
                  Text(childTitle_),
                  started ? result->output
                          : Filled(Text(IDS_CHILD_NOT_STARTED), L"{hr}", HResultText(result->hr)));
  if (started && result->exitCode == 0 && closeAfterChild_) {
    DestroyWindow(dialog_);
    return;
  }
  RequestReading();
  RequestPreview();
}

void MainWindow::OnSave() {
  // Enter does not move the focus, so the box has not settled what it shows yet.
  OnZoomBoxLeft();
  if (!DecideControls(state_).save) return;
  SaveRequest request;
  if (edits_.zoom != saved_.zoom) request.zoom = edits_.zoom;
  request.changeChecks =
      edits_.afterPdfCraft != saved_.afterPdfCraft || edits_.atSignIn != saved_.atSignIn;
  request.afterPdfCraft = edits_.afterPdfCraft;
  request.atSignIn = edits_.atSignIn;
  panelBeforeCheck_ = panel_;
  panel_.lines[0] = {StatusGlyph::kNone, Text(IDS_SAVING)};
  message_.clear();
  state_.activity = WindowActivity::kSaving;
  ApplyState();
  jobs_->StartSave(context_, request);
}

void MainWindow::OnCancelRefresh() {
  if (state_.activity != WindowActivity::kRefreshing) return;
  jobs_->RequestStop();
  state_.activity = WindowActivity::kStopping;
  panel_.lines[0] = {StatusGlyph::kNone, Text(IDS_STOPPING)};
  ApplyState();
}

void MainWindow::OnRefreshProgress() {
  const std::optional<RefreshProgress> progress = jobs_->TakeProgress();
  if (!progress) return;
  if (state_.activity == WindowActivity::kSaving) {
    state_.activity = WindowActivity::kRefreshing;
  } else if (state_.activity != WindowActivity::kRefreshing) {
    return;  // being stopped, or over: this one was on its way
  }
  const HWND bar = Item(IDC_PROGRESS);
  const bool counting = progress->phase == RefreshPhase::kRefreshing && progress->files > 0;
  const LONG_PTR style = GetWindowLongPtrW(bar, GWL_STYLE);
  SetWindowLongPtrW(bar, GWL_STYLE, counting ? style & ~PBS_MARQUEE : style | PBS_MARQUEE);
  if (counting) {
    SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
    SendMessageW(bar, PBM_SETRANGE32, 0, static_cast<LPARAM>(progress->files));
    SendMessageW(bar, PBM_SETPOS, progress->done, 0);
  } else {
    SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
  }
  const std::wstring text = RefreshProgressText(*progress);
  if (!text.empty() && !closePending_) panel_.lines[0] = {StatusGlyph::kNone, text};
  ApplyState();
}

void MainWindow::OnCheckBoxChanged() {
  edits_.afterPdfCraft = IsDlgButtonChecked(dialog_, IDC_CHECK_PDFCRAFT) == BST_CHECKED;
  edits_.atSignIn = IsDlgButtonChecked(dialog_, IDC_CHECK_SIGNIN) == BST_CHECKED;
  state_.dirty = edits_ != saved_;
  ApplyState();
}

void MainWindow::ShowNote(std::wstring note) {
  message_ = std::move(note);
  ApplyState();
}

void MainWindow::OnOpenLog() {
  if (!DecideControls(state_).openLogAndDiagnostics) return;
  std::filesystem::path log;
  if (const HRESULT hr = GetLogPath(&log); FAILED(hr)) {
    ShowNote(Filled(Text(IDS_LOG_FAILED), L"{hr}", HResultText(hr)));
    return;
  }
  if (GetFileAttributesW(log.c_str()) == INVALID_FILE_ATTRIBUTES) {
    ShowNote(Text(IDS_LOG_MISSING));
    return;
  }
  const auto opened = reinterpret_cast<INT_PTR>(
      ShellExecuteW(dialog_, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (opened <= 32) {
    ShowNote(Filled(Text(IDS_LOG_FAILED), L"{hr}",
                    HResultText(HRESULT_FROM_WIN32(static_cast<DWORD>(opened)))));
  }
}

void MainWindow::OnCopyDiagnostics() {
  if (!DecideControls(state_).openLogAndDiagnostics || diagnosticsJob_->running()) return;
  diagnosticsJob_->StartDiagnostics(context_);
  ApplyState();
}

void MainWindow::OnDiagnosticsDone() {
  const std::optional<StateReading> reading = diagnosticsJob_->TakeReading();
  HRESULT hr = reading ? reading->hr : E_UNEXPECTED;
  if (SUCCEEDED(hr)) hr = CopyTextToClipboard(ToCrLf(reading->diagnostics));
  ShowNote(SUCCEEDED(hr) ? Text(IDS_DIAG_COPIED)
                         : Filled(Text(IDS_DIAG_FAILED), L"{hr}", HResultText(hr)));
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
  std::optional<SaveReport> saved;
  if (reading && reading->save) {
    panel_ = panelBeforeCheck_;
    saved = DescribeSave(*reading->save, kPdfRefreshDefault);
    message_ = saved->note;
    if (saved->busy) {
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
  // A part that failed stays unsaved, so the window stays open to say so.
  if (saved && (saved->busy || !saved->problem.empty())) {
    closePending_ = false;
    if (!saved->problem.empty()) {
      const ModalScope question(&questionsOpen_);
      ShowProblem(dialog_, saved->problem);
    }
  } else if constexpr (!kPdfRefreshDefault) {
    // The thumbnails already made keep the old zoom until they are made again.
    if (saved && reading->save->zoomAsked && !closePending_) OnRebuild();
  }
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
    } else if (state_.context == WindowContext::kStarting) {
      state_.context = WindowContext::kUnreadable;
    }
    return;
  }
  if (!reading.snapshotRead) return;
  state_.context = ClassifyWindowContext(RefusesWrites(context_.virtualization),
                                         reading.snapshot.scope != Scope::kNone);
  panel_ = BuildStatusPanel(reading.snapshot, reading.findings);
  canChangeChecks_ = InstalledSettingsExe(reading.snapshot).has_value();
  confirmFullRefresh_ = reading.snapshot.settings.confirmFullRefresh;
  SendDlgItemMessageW(dialog_, IDC_UNINSTALL, BCM_SETSHIELD, 0,
                      reading.snapshot.machineClass.has_value() ? TRUE : FALSE);
  SendDlgItemMessageW(dialog_, IDC_INSTALL_MACHINE, BCM_SETSHIELD, 0, TRUE);
  SetText(IDC_CHECK_PDFCRAFT,
          pdfCraftCheckText_ +
              (reading.snapshot.pdfcraft.installed ? L"" : Text(IDS_PDFCRAFT_NOT_FOUND)));
  // Someone's unsaved edits stay; only what they are compared with moves.
  saved_ = {reading.snapshot.settings.zoom, reading.snapshot.task.afterPdfCraft,
            reading.snapshot.task.atSignIn};
  if (state_.dirty) {
    state_.dirty = edits_ != saved_;
    return;
  }
  edits_ = saved_;
  ShowZoom(edits_.zoom);
  DescribeTiles();
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
  SetEnabled(IDC_CHECK_PDFCRAFT, controls.zoomAndChecks && canChangeChecks_);
  SetEnabled(IDC_CHECK_SIGNIN, controls.zoomAndChecks && canChangeChecks_);
  SetEnabled(IDOK, controls.save);
  SetVisible(IDC_PROGRESS, state_.activity == WindowActivity::kRefreshing ||
                               state_.activity == WindowActivity::kStopping);
  SetVisible(IDC_PROGRESS_CANCEL, controls.cancelLink);
  SetEnabled(IDC_CHECK_NOW, controls.checkNowAndFix);
  SetEnabled(IDC_REBUILD, controls.rebuild);
  SetEnabled(IDC_UNINSTALL, controls.uninstall);
  // In a developer build with nothing installed, the buttons take the place of the status lines.
  const bool installButtons = kDevInstall && controls.install;
  SetVisible(IDC_INSTALL_MACHINE, installButtons);
  SetVisible(IDC_INSTALL_USER, installButtons);
  for (size_t i = 0; i < kLineControls.size(); ++i) {
    const StatusGlyph glyph = panel_.lines[i].glyph;
    SetVisible(kLineControls[i], !installButtons);
    // A glyph that says nothing must not be there for a screen reader to stop at.
    SetVisible(kGlyphControls[i], !installButtons && glyph != StatusGlyph::kNone);
    if (glyph != StatusGlyph::kNone) {
      accessibility_.SetName(Item(kGlyphControls[i]),
                             Text(glyph == StatusGlyph::kOk        ? IDS_NAME_GLYPH_OK
                                  : glyph == StatusGlyph::kWarning ? IDS_NAME_GLYPH_WARNING
                                                                   : IDS_NAME_GLYPH_PROBLEM));
    }
  }
  SetEnabled(IDC_OPEN_LOG, controls.openLogAndDiagnostics);
  SetEnabled(IDC_COPY_DIAG, controls.openLogAndDiagnostics && !diagnosticsJob_->running());

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
  }
  // With the Fix link in the note's place, the banner's row carries the note if it is free.
  if (banner == WindowBanner::kNone && showFix) text = note;
  SetText(IDC_BANNER, text);
  SetVisible(IDC_BANNER, !text.empty());
  if (text != announcedBanner_) {
    announcedBanner_ = text;
    if (!text.empty()) accessibility_.Announce(Item(IDC_BANNER));
  }
  const ULONGLONG now = GetTickCount64();
  if (panel_.lines[0].text != announcedLine_ &&
      (state_.activity != WindowActivity::kRefreshing || now - announcedAt_ >= 1000)) {
    announcedLine_ = panel_.lines[0].text;
    announcedAt_ = now;
    accessibility_.Announce(Item(IDC_STATUS_LINE1));
  }
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
  ApplyState();
}

void MainWindow::RequestClose() {
  if (questionsOpen_ > 0) return;  // the question that is open is answered first
  switch (DecideClose(state_)) {
    case CloseAction::kCloseNow:
      DestroyWindow(dialog_);
      break;
    case CloseAction::kAskSaveChanges: {
      SaveAnswer answer = SaveAnswer::kCancel;
      {
        const ModalScope question(&questionsOpen_);
        answer = AskSaveChanges(dialog_);
      }
      switch (answer) {
        case SaveAnswer::kCancel:
          return;
        case SaveAnswer::kSave:
          if (!DecideControls(state_).save) return;  // busy or still reading: nothing lost
          closePending_ = true;
          OnSave();
          return;
        case SaveAnswer::kDontSave:
          break;
      }
      if (!jobs_->running()) {
        DestroyWindow(dialog_);
        return;
      }
      closePending_ = true;
      return;
    }
    case CloseAction::kAskStopRefresh: {
      {
        const ModalScope question(&questionsOpen_);
        if (!ConfirmStopRefresh(dialog_)) return;
      }
      // The refresh may have ended while the question was open.
      if (state_.activity != WindowActivity::kRefreshing) {
        if (!jobs_->running()) {
          DestroyWindow(dialog_);
        } else {
          closePending_ = true;
        }
        return;
      }
      closePending_ = true;
      OnCancelRefresh();
      return;
    }
    case CloseAction::kWaitForWork:
      closePending_ = true;
      panel_.lines[0] = {StatusGlyph::kNone, Text(IDS_CLOSING_AFTER_WORK)};
      ApplyState();
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
