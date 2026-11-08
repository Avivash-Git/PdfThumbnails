#include "search_index.h"

// oledb.h before msdasc.h, which uses its types.
#include <msdasc.h>
#include <objbase.h>
#include <oledb.h>
#include <oledberr.h>
#include <searchapi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "log.h"
#include "strings.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"refresh";
constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);

// How long Windows Search is given, in all.
constexpr DWORD kIndexWaitMs = 30000;
// How often the wait looks at the stop.
constexpr DWORD kIndexPollMs = 100;
// Rows asked for at a time.
constexpr DBROWCOUNT kRowBatch = 256;

// DBGUID_DEFAULT, here so that no import library of OLE DB's GUIDs is needed.
constexpr GUID kDbGuidDefault = {
    0xc8b521fb, 0x5cf3, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};

// What the caller and the worker thread share: each holds a reference, so it lives until the later
// of the two lets go, also after a timeout. The caller reads the results only after |finished|.
struct IndexJob {
  size_t cap = 0;
  std::wstring query;      // the SQL; empty: the scope question
  std::wstring folderUrl;  // the scope question's folder
  std::stop_token stop;
  std::atomic<bool> abandoned{false};  // nobody waits any more
  HRESULT hr = E_PENDING;
  std::vector<std::filesystem::path> files;
  bool included = false;
  wil::unique_event_nothrow finished;
};

bool ShouldStop(const IndexJob& job) noexcept { return job.stop.stop_requested() || job.abandoned; }

HRESULT OpenCatalog(wil::com_ptr_nothrow<ISearchCatalogManager>* catalog) noexcept {
  wil::com_ptr_nothrow<ISearchManager> manager;
  RETURN_IF_FAILED(CoCreateInstance(__uuidof(CSearchManager), nullptr, CLSCTX_LOCAL_SERVER,
                                    IID_PPV_ARGS(manager.put())));
  RETURN_IF_FAILED(manager->GetCatalog(L"SystemIndex", catalog->put()));
  return S_OK;
}

// One row's path, as the provider holds it until the row is released.
struct IndexRow {
  DBLENGTH length = 0;  // in bytes
  DBSTATUS status = 0;
  const wchar_t* path = nullptr;
};

// Runs |job|'s query and reads the rows, at most |job|.cap, looking at the stop between batches.
HRESULT RunQuery(IndexJob* job) noexcept try {
  wil::com_ptr_nothrow<ISearchCatalogManager> catalog;
  RETURN_IF_FAILED(OpenCatalog(std::addressof(catalog)));
  wil::com_ptr_nothrow<ISearchQueryHelper> helper;
  RETURN_IF_FAILED(catalog->GetQueryHelper(helper.put()));
  wil::unique_cotaskmem_string connection;
  RETURN_IF_FAILED(helper->get_ConnectionString(connection.put()));

  wil::com_ptr_nothrow<IDataInitialize> data;
  RETURN_IF_FAILED(CoCreateInstance(__uuidof(MSDAINITIALIZE), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(data.put())));
  wil::com_ptr_nothrow<IDBInitialize> source;
  RETURN_IF_FAILED(data->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, connection.get(),
                                       __uuidof(IDBInitialize), source.put_unknown()));
  RETURN_IF_FAILED(source->Initialize());
  wil::com_ptr_nothrow<IDBCreateSession> sessions;
  RETURN_IF_FAILED(source.query_to(&sessions));
  wil::com_ptr_nothrow<IDBCreateCommand> session;
  RETURN_IF_FAILED(
      sessions->CreateSession(nullptr, __uuidof(IDBCreateCommand), session.put_unknown()));
  wil::com_ptr_nothrow<ICommandText> command;
  RETURN_IF_FAILED(session->CreateCommand(nullptr, __uuidof(ICommandText), command.put_unknown()));
  RETURN_IF_FAILED(command->SetCommandText(kDbGuidDefault, job->query.c_str()));
  wil::com_ptr_nothrow<IRowset> rowset;
  RETURN_IF_FAILED(
      command->Execute(nullptr, __uuidof(IRowset), nullptr, nullptr, rowset.put_unknown()));

  DBBINDING binding = {};
  binding.iOrdinal = 1;
  binding.obValue = offsetof(IndexRow, path);
  binding.obLength = offsetof(IndexRow, length);
  binding.obStatus = offsetof(IndexRow, status);
  binding.dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
  binding.dwMemOwner = DBMEMOWNER_PROVIDEROWNED;
  binding.eParamIO = DBPARAMIO_NOTPARAM;
  binding.cbMaxLen = sizeof(wchar_t*);
  binding.wType = DBTYPE_WSTR | DBTYPE_BYREF;
  wil::com_ptr_nothrow<IAccessor> accessor;
  RETURN_IF_FAILED(rowset.query_to(&accessor));
  HACCESSOR handle = DB_NULL_HACCESSOR;
  RETURN_IF_FAILED(accessor->CreateAccessor(DBACCESSOR_ROWDATA, 1, &binding, sizeof(IndexRow),
                                            &handle, nullptr));
  const auto releaseAccessor = wil::scope_exit(
      [&accessor, handle]() noexcept { (void)accessor->ReleaseAccessor(handle, nullptr); });

  HROW rows[kRowBatch] = {};
  for (;;) {
    if (ShouldStop(*job)) return kCancelled;
    DBCOUNTITEM obtained = 0;
    HROW* into = rows;
    const HRESULT next = rowset->GetNextRows(DB_NULL_HCHAPTER, 0, kRowBatch, &obtained, &into);
    RETURN_IF_FAILED(next);
    if (obtained == 0) return S_OK;
    const auto releaseRows = wil::scope_exit([&rowset, &rows, obtained]() noexcept {
      (void)rowset->ReleaseRows(obtained, rows, nullptr, nullptr, nullptr);
    });
    for (DBCOUNTITEM i = 0; i < obtained && job->files.size() < job->cap; ++i) {
      IndexRow row;
      if (SUCCEEDED(rowset->GetData(rows[i], handle, &row)) && row.status == DBSTATUS_S_OK &&
          row.path != nullptr && row.length > 0) {
        job->files.emplace_back(std::wstring(row.path, row.length / sizeof(wchar_t)));
      }
    }
    if (job->files.size() >= job->cap || next == DB_S_ENDOFROWSET) return S_OK;
  }
}
CATCH_RETURN();

HRESULT RunScopeQuestion(IndexJob* job) noexcept {
  wil::com_ptr_nothrow<ISearchCatalogManager> catalog;
  RETURN_IF_FAILED(OpenCatalog(std::addressof(catalog)));
  wil::com_ptr_nothrow<ISearchCrawlScopeManager> scope;
  RETURN_IF_FAILED(catalog->GetCrawlScopeManager(scope.put()));
  BOOL included = FALSE;
  RETURN_IF_FAILED(scope->IncludedInCrawlScope(job->folderUrl.c_str(), &included));
  job->included = included != FALSE;
  return S_OK;
}

DWORD WINAPI IndexJobThread(void* parameter) noexcept {
  std::shared_ptr<IndexJob> job;
  {
    // RunIndexJob handed over a reference of its own on the heap; it is the thread's now.
    std::unique_ptr<std::shared_ptr<IndexJob>> handoff(
        static_cast<std::shared_ptr<IndexJob>*>(parameter));
    job = std::move(*handoff);
  }
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(init)) {
    job->hr = job->query.empty() ? RunScopeQuestion(job.get()) : RunQuery(job.get());
    CoUninitialize();
  } else {
    job->hr = init;
  }
  SetEvent(job->finished.get());
  return 0;
}

// Runs |job| on a thread of its own. S_OK when it finished (its result in job->hr); S_FALSE when
// the time ran out; kCancelled when stopped; a failure when no thread could be started.
HRESULT RunIndexJob(const std::shared_ptr<IndexJob>& job) noexcept try {
  RETURN_IF_FAILED(job->finished.create(wil::EventOptions::ManualReset));
  auto handoff = std::make_unique<std::shared_ptr<IndexJob>>(job);
  wil::unique_handle thread(CreateThread(nullptr, 0, IndexJobThread, handoff.get(), 0, nullptr));
  RETURN_LAST_ERROR_IF(!thread);
  handoff.release();  // IndexJobThread deletes it
  for (DWORD waited = 0; waited < kIndexWaitMs; waited += kIndexPollMs) {
    const DWORD wait = WaitForSingleObject(job->finished.get(), kIndexPollMs);
    if (wait == WAIT_OBJECT_0) return S_OK;
    if (wait != WAIT_TIMEOUT) {
      job->abandoned = true;
      RETURN_LAST_ERROR();
    }
    if (job->stop.stop_requested()) {
      job->abandoned = true;
      return kCancelled;
    }
  }
  job->abandoned = true;
  return S_FALSE;
}
CATCH_RETURN();

// |text| with every ' doubled, for a string in the query.
std::wstring Quoted(const std::wstring& text) {
  std::wstring result;
  result.reserve(text.size());
  for (const wchar_t c : text) {
    result += c;
    if (c == L'\'') result += c;
  }
  return result;
}

void LogUnavailable(HRESULT hr) noexcept try {
  Log(LogLevel::kInfo, kLogComponent, L"refresh index unavailable hr=" + HResultText(hr));
} catch (...) {
  // Logging is best effort.
}

}  // namespace

HRESULT QueryIndexedPdfs(size_t cap, const std::filesystem::path* under, std::stop_token stop,
                         std::vector<std::filesystem::path>* files, bool* available) noexcept try {
  files->clear();
  *available = false;
  if (stop.stop_requested()) return kCancelled;
  auto job = std::make_shared<IndexJob>();
  job->cap = cap;
  job->stop = stop;
  job->query = L"SELECT System.ItemPathDisplay FROM SystemIndex WHERE SCOPE='file:" +
               (under ? Quoted(under->native()) : std::wstring()) +
               L"' AND System.FileExtension='.pdf' ORDER BY System.DateModified DESC";
  const HRESULT ran = RunIndexJob(job);
  RETURN_IF_FAILED(ran);
  if (ran == S_FALSE) {
    LogUnavailable(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    return S_OK;
  }
  if (job->hr == kCancelled) return kCancelled;
  if (job->hr == E_OUTOFMEMORY) return E_OUTOFMEMORY;
  if (FAILED(job->hr)) {
    LogUnavailable(job->hr);
    return S_OK;
  }
  *files = std::move(job->files);
  *available = true;
  return S_OK;
}
CATCH_RETURN();

HRESULT IsInIndexScope(const std::filesystem::path& folder, bool* included) noexcept try {
  *included = false;
  auto job = std::make_shared<IndexJob>();
  job->folderUrl = L"file:///" + folder.native();
  const HRESULT ran = RunIndexJob(job);
  RETURN_IF_FAILED(ran);
  if (ran == S_FALSE) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  RETURN_IF_FAILED(job->hr);
  *included = job->included;
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
