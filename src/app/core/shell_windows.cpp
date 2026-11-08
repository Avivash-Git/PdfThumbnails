#include "shell_windows.h"

// oleauto.h before (another inclusion of) wil/resource.h enables WIL's BSTR and VARIANT wrappers.
#include <oleauto.h>
#include <shlobj.h>  // ShellWindows (exdisp.h), IShellDispatch2 (shldisp.h), SID_STopLevelBrowser
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <atomic>
#include <memory>
#include <utility>

#include "processes.h"

// shellapi.h's macro of this name would rename IShellDispatch2::ShellExecute below.
#undef ShellExecute

namespace pdft {
namespace {

// What every call gives while no shell window exists: an answer for the caller, not a failure
// worth logging.
constexpr HRESULT kNoShell = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

// What the caller and the worker thread share (the Explorer test's pattern, live_test.cpp). The
// caller and the thread each hold a reference (std::shared_ptr), so the job lives until the later
// of the two lets go: after a timeout the caller returns and drops its reference, and the worker,
// still waiting on File Explorer, keeps using memory that stays valid until it ends. The caller
// reads the results only after |finished| is set, which orders the worker's writes before the
// read, except for the atomic counters, which it may read after a timeout. |abandoned| tells the
// worker that nobody waits any more, so it starts nothing new.
class ShellJob {
 public:
  virtual ~ShellJob() = default;
  // The work, on the worker thread in an apartment of its own. Every COM pointer it uses is
  // released before it returns.
  virtual HRESULT Run() noexcept = 0;

  HRESULT hr = E_PENDING;  // what Run returned
  std::atomic<bool> abandoned{false};
  wil::unique_event_nothrow finished;
};

DWORD WINAPI ShellJobThread(void* parameter) noexcept {
  std::shared_ptr<ShellJob> job;
  {
    // RunShellJob handed over a reference of its own on the heap; it is the thread's now.
    std::unique_ptr<std::shared_ptr<ShellJob>> handoff(
        static_cast<std::shared_ptr<ShellJob>*>(parameter));
    job = std::move(*handoff);
  }
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (SUCCEEDED(init)) {
    job->hr = job->Run();
    CoUninitialize();
  } else {
    job->hr = init;
  }
  SetEvent(job->finished.get());
  return 0;
}

// Runs |job| on a thread of its own and waits at most |timeoutMs|. S_OK when it finished (its
// result in job->hr); S_FALSE when the time ran out, and the job, abandoned, finishes on its own;
// a failure when no thread could be started.
HRESULT RunShellJob(const std::shared_ptr<ShellJob>& job, DWORD timeoutMs) noexcept try {
  RETURN_IF_FAILED(job->finished.create(wil::EventOptions::ManualReset));
  auto handoff = std::make_unique<std::shared_ptr<ShellJob>>(job);
  wil::unique_handle thread(CreateThread(nullptr, 0, ShellJobThread, handoff.get(), 0, nullptr));
  RETURN_LAST_ERROR_IF(!thread);
  handoff.release();  // ShellJobThread deletes it
  const DWORD wait = WaitForSingleObject(job->finished.get(), timeoutMs);
  if (wait == WAIT_OBJECT_0) return S_OK;
  job->abandoned = true;
  if (wait == WAIT_TIMEOUT) return S_FALSE;
  RETURN_LAST_ERROR();
}
CATCH_RETURN();

// File Explorer's own ShellWindows (a local server, never an in-process copy that knows no
// windows).
HRESULT CreateShellWindows(wil::com_ptr_nothrow<IShellWindows>* shellWindows) noexcept {
  RETURN_IF_FAILED(CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER,
                                    IID_PPV_ARGS(shellWindows->put())));
  return S_OK;
}

// The window at |index| of the list; nothing when it closed in the meantime.
wil::com_ptr_nothrow<IDispatch> WindowAt(IShellWindows* shellWindows, long index) noexcept {
  VARIANT position = {};
  position.vt = VT_I4;
  position.lVal = index;
  wil::com_ptr_nothrow<IDispatch> window;
  if (shellWindows->Item(position, window.put()) != S_OK) window.reset();
  return window;
}

// The shell browser of |window|; a failure for a window that is not a folder window. Not logged:
// the list may hold other windows.
HRESULT BrowserOf(IUnknown* window, wil::com_ptr_nothrow<IShellBrowser>* browser) noexcept {
  browser->reset();
  wil::com_ptr_nothrow<IServiceProvider> provider;
  HRESULT hr = window->QueryInterface(IID_PPV_ARGS(provider.put()));
  if (SUCCEEDED(hr)) {
    hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser->put()));
  }
  if (SUCCEEDED(hr) && !*browser) hr = E_NOINTERFACE;
  return hr;
}

// The view |browser| shows now; a failure while it shows none (between two folders). Not logged.
HRESULT ActiveView(IShellBrowser* browser, wil::com_ptr_nothrow<IShellView>* view) noexcept {
  view->reset();
  HRESULT hr = browser->QueryActiveShellView(view->put());
  if (SUCCEEDED(hr) && !*view) hr = E_NOINTERFACE;
  return hr;
}

// The name of the folder |view| shows, of the kind |kind|; a failure when it has none of that
// kind (SIGDN_FILESYSPATH of This PC). Not logged.
HRESULT FolderName(IShellView* view, SIGDN kind, std::wstring* name) noexcept try {
  name->clear();
  wil::com_ptr_nothrow<IFolderView> folderView;
  HRESULT hr = view->QueryInterface(IID_PPV_ARGS(folderView.put()));
  wil::com_ptr_nothrow<IPersistFolder2> folder;
  if (SUCCEEDED(hr)) hr = folderView->GetFolder(IID_PPV_ARGS(folder.put()));
  wil::unique_cotaskmem_ptr<ITEMIDLIST_ABSOLUTE> idList;
  if (SUCCEEDED(hr)) hr = folder->GetCurFolder(wil::out_param(idList));
  if (SUCCEEDED(hr) && !idList) hr = E_UNEXPECTED;
  wil::unique_cotaskmem_string text;
  if (SUCCEEDED(hr)) hr = SHGetNameFromIDList(idList.get(), kind, text.put());
  if (SUCCEEDED(hr) && !text) hr = E_UNEXPECTED;
  if (FAILED(hr)) return hr;
  *name = text.get();
  return S_OK;
}
CATCH_RETURN();

// The top-level window |window| is in; 0 when it does not say.
ULONG_PTR FrameOf(IUnknown* window) noexcept {
  wil::com_ptr_nothrow<IWebBrowserApp> app;
  SHANDLE_PTR frame = 0;
  if (FAILED(window->QueryInterface(IID_PPV_ARGS(app.put()))) || FAILED(app->get_HWND(&frame))) {
    return 0;
  }
  return static_cast<ULONG_PTR>(frame);
}

// The desktop's shell browser and the view it shows (FindWindowSW with SWC_DESKTOP: the desktop
// is not in the list of windows).
HRESULT DesktopView(IShellWindows* shellWindows, wil::com_ptr_nothrow<IShellBrowser>* browser,
                    wil::com_ptr_nothrow<IShellView>* view) noexcept {
  VARIANT location = {};
  location.vt = VT_I4;
  location.lVal = CSIDL_DESKTOP;
  VARIANT root = {};
  root.vt = VT_EMPTY;
  long window = 0;
  wil::com_ptr_nothrow<IDispatch> desktop;
  const HRESULT hr = shellWindows->FindWindowSW(&location, &root, SWC_DESKTOP, &window,
                                                SWFO_NEEDDISPATCH, desktop.put());
  RETURN_IF_FAILED(hr);
  if (hr == S_FALSE || !desktop) return kNoShell;  // S_FALSE: no desktop window
  RETURN_IF_FAILED(BrowserOf(desktop.get(), browser));
  RETURN_IF_FAILED(ActiveView(browser->get(), view));
  return S_OK;
}

class ListJob final : public ShellJob {
 public:
  HRESULT Run() noexcept override try {
    wil::com_ptr_nothrow<IShellWindows> shellWindows;
    RETURN_IF_FAILED(CreateShellWindows(std::addressof(shellWindows)));
    long count = 0;
    RETURN_IF_FAILED(shellWindows->get_Count(&count));
    for (long i = 0; i < count && !abandoned.load(); ++i) {
      const wil::com_ptr_nothrow<IDispatch> item = WindowAt(shellWindows.get(), i);
      wil::com_ptr_nothrow<IShellBrowser> browser;
      if (!item || FAILED(BrowserOf(item.get(), std::addressof(browser)))) continue;
      FolderWindow window;
      window.frame = FrameOf(item.get());
      wil::com_ptr_nothrow<IShellView> view;
      if (SUCCEEDED(ActiveView(browser.get(), std::addressof(view)))) {
        (void)FolderName(view.get(), SIGDN_DESKTOPABSOLUTEPARSING, &window.parsingName);
      }
      windows.push_back(std::move(window));
    }
    return S_OK;
  }
  CATCH_RETURN();

  std::vector<FolderWindow> windows;
};

// The running shell's automation object, from the desktop view: it belongs to explorer.exe, and
// so does what it opens or starts. kNoShell when no desktop can be reached. The shell may bring
// what it opens to the front.
HRESULT RunningShell(wil::com_ptr_nothrow<IShellDispatch2>* shell) noexcept {
  wil::com_ptr_nothrow<IShellWindows> shellWindows;
  RETURN_IF_FAILED(CreateShellWindows(std::addressof(shellWindows)));
  wil::com_ptr_nothrow<IShellBrowser> browser;
  wil::com_ptr_nothrow<IShellView> view;
  const HRESULT reached =
      DesktopView(shellWindows.get(), std::addressof(browser), std::addressof(view));
  if (reached == kNoShell) return reached;
  RETURN_IF_FAILED(reached);
  wil::com_ptr_nothrow<IDispatch> background;
  RETURN_IF_FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(background.put())));
  wil::com_ptr_nothrow<IShellFolderViewDual> folderView;
  RETURN_IF_FAILED(background.query_to(folderView.put()));
  wil::com_ptr_nothrow<IDispatch> application;
  RETURN_IF_FAILED(folderView->get_Application(application.put()));
  RETURN_HR_IF_NULL(E_NOINTERFACE, application);
  RETURN_IF_FAILED(application.query_to(shell->put()));
  DWORD shellPid = 0;
  if (GetWindowThreadProcessId(GetShellWindow(), &shellPid) && shellPid) {
    (void)AllowSetForegroundWindow(shellPid);
  }
  return S_OK;
}

class StartJob final : public ShellJob {
 public:
  HRESULT Run() noexcept override try {
    wil::com_ptr_nothrow<IShellDispatch2> shell;
    const HRESULT reached = RunningShell(std::addressof(shell));
    if (reached == kNoShell) return reached;
    RETURN_IF_FAILED(reached);
    if (abandoned.load()) return S_OK;
    wil::unique_bstr file(SysAllocString(program.c_str()));
    RETURN_IF_NULL_ALLOC(file);
    wil::unique_variant args = wil::make_variant_bstr_nothrow(arguments.c_str());
    if (args.vt != VT_BSTR) return E_OUTOFMEMORY;
    wil::unique_variant none;  // the shell's own folder, its default verb
    wil::unique_variant show;
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    RETURN_IF_FAILED(shell->ShellExecute(file.get(), args, none, none, show));
    return S_OK;
  }
  CATCH_RETURN();

  std::wstring program;
  std::wstring arguments;
};

class OpenJob final : public ShellJob {
 public:
  HRESULT Run() noexcept override try {
    wil::com_ptr_nothrow<IShellDispatch2> shell;
    const HRESULT reached = RunningShell(std::addressof(shell));
    if (reached == kNoShell) return reached;
    RETURN_IF_FAILED(reached);
    for (const FolderWindow& window : windows) {
      if (abandoned.load()) break;
      if (window.parsingName.empty()) continue;
      wil::unique_variant folder = wil::make_variant_bstr_nothrow(window.parsingName.c_str());
      if (folder.vt != VT_BSTR) continue;  // out of memory: not opened
      // S_FALSE when the shell did not open it (no such folder).
      if (shell->Open(folder) == S_OK) ++opened;
    }
    return S_OK;
  }
  CATCH_RETURN();

  std::vector<FolderWindow> windows;
  std::atomic<size_t> opened{0};
};

class ViewsJob final : public ShellJob {
 public:
  HRESULT Run() noexcept override try {
    wil::com_ptr_nothrow<IShellWindows> shellWindows;
    RETURN_IF_FAILED(CreateShellWindows(std::addressof(shellWindows)));
    long count = 0;
    RETURN_IF_FAILED(shellWindows->get_Count(&count));
    for (long i = 0; i < count && !abandoned.load(); ++i) {
      const wil::com_ptr_nothrow<IDispatch> item = WindowAt(shellWindows.get(), i);
      wil::com_ptr_nothrow<IShellBrowser> browser;
      wil::com_ptr_nothrow<IShellView> view;
      std::wstring folder;
      if (!item || FAILED(BrowserOf(item.get(), std::addressof(browser))) ||
          FAILED(ActiveView(browser.get(), std::addressof(view))) ||
          FAILED(FolderName(view.get(), SIGDN_FILESYSPATH, &folder)) || !Wanted(folder)) {
        continue;
      }
      // The caller may have stopped waiting while File Explorer answered the calls above.
      if (abandoned.load()) break;
      ++found;
      action(browser.get(), view.get());
    }
    if (desktop && !abandoned.load()) {
      wil::com_ptr_nothrow<IShellBrowser> browser;
      wil::com_ptr_nothrow<IShellView> view;
      const HRESULT reached =
          DesktopView(shellWindows.get(), std::addressof(browser), std::addressof(view));
      if (reached == kNoShell) return reached;
      RETURN_IF_FAILED(reached);
      if (abandoned.load()) return S_OK;
      ++found;
      action(browser.get(), view.get());
    }
    return S_OK;
  }
  CATCH_RETURN();

  std::vector<std::filesystem::path> folders;
  bool desktop = false;
  ViewAction action;
  std::atomic<size_t> found{0};

 private:
  bool Wanted(const std::wstring& folder) const noexcept {
    for (const std::filesystem::path& wanted : folders) {
      if (SameImagePath(folder, wanted.native())) return true;
    }
    return false;
  }
};

}  // namespace

HRESULT ListFolderWindows(DWORD timeoutMs, std::vector<FolderWindow>* windows) noexcept try {
  windows->clear();
  if (!GetShellWindow()) return kNoShell;
  auto job = std::make_shared<ListJob>();
  const HRESULT ran = RunShellJob(job, timeoutMs);
  RETURN_IF_FAILED(ran);
  if (ran == S_FALSE) return S_FALSE;
  if (FAILED(job->hr)) return job->hr;
  *windows = std::move(job->windows);
  return S_OK;
}
CATCH_RETURN();

HRESULT OpenFolderWindows(std::span<const FolderWindow> windows, DWORD timeoutMs, size_t* opened,
                          size_t* failed) noexcept try {
  *opened = 0;
  *failed = windows.size();
  if (windows.empty()) return S_OK;
  if (!GetShellWindow()) return kNoShell;
  auto job = std::make_shared<OpenJob>();
  job->windows.assign(windows.begin(), windows.end());
  const HRESULT ran = RunShellJob(job, timeoutMs);
  RETURN_IF_FAILED(ran);
  *opened = job->opened.load();
  *failed = windows.size() - *opened;
  return ran == S_FALSE ? S_FALSE : job->hr;
}
CATCH_RETURN();

HRESULT StartThroughShell(const std::filesystem::path& program, std::wstring_view arguments,
                          DWORD timeoutMs) noexcept try {
  if (!GetShellWindow()) return kNoShell;
  auto job = std::make_shared<StartJob>();
  job->program = program.native();
  job->arguments = arguments;
  const HRESULT ran = RunShellJob(job, timeoutMs);
  RETURN_IF_FAILED(ran);
  return ran == S_FALSE ? S_FALSE : job->hr;
}
CATCH_RETURN();

HRESULT ForEachViewShowing(std::span<const std::filesystem::path> folders, bool desktop,
                           DWORD timeoutMs, const ViewAction& action, size_t* found) noexcept try {
  *found = 0;
  RETURN_HR_IF(E_INVALIDARG, !action);
  if (folders.empty() && !desktop) return S_OK;
  if (!GetShellWindow()) return kNoShell;
  auto job = std::make_shared<ViewsJob>();
  job->folders.assign(folders.begin(), folders.end());
  job->desktop = desktop;
  job->action = action;
  const HRESULT ran = RunShellJob(job, timeoutMs);
  RETURN_IF_FAILED(ran);
  *found = job->found.load();
  return ran == S_FALSE ? S_FALSE : job->hr;
}
CATCH_RETURN();

}  // namespace pdft
