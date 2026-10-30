// The COM surface and every failure the 1.1 HRESULT table names that a caller can provoke:
// bad arguments, wrong call order, empty or oversized streams, streams that fail.

#include <windows.h>

#include <gtest/gtest.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstring>

#include "handler_env.h"
#include "project_config.h"

namespace pdft::test {
namespace {

// A read-only IStream whose Stat, Seek and Read results are set by the test.
class FakeStream final : public IStream {
 public:
  ULONGLONG size = 0;
  bool statFails = false;
  bool seekFails = false;
  bool readFails = false;
  bool readsNothing = false;

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
      *object = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&refs_); }  // on stack
  LONG refs() const { return refs_; }

  // ISequentialStream
  STDMETHODIMP Read(void* buffer, ULONG count, ULONG* read) override {
    if (read) *read = 0;
    if (readFails) return STG_E_READFAULT;
    if (readsNothing || position_ >= size) return S_OK;
    const ULONG n = static_cast<ULONG>(std::min<ULONGLONG>(count, size - position_));
    memset(buffer, ' ', n);
    position_ += n;
    if (read) *read = n;
    return S_OK;
  }
  STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

  // IStream
  STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
    if (seekFails) return STG_E_INVALIDFUNCTION;
    const ULONGLONG base = origin == STREAM_SEEK_SET   ? 0
                           : origin == STREAM_SEEK_CUR ? position_
                                                       : size;
    position_ = base + move.QuadPart;
    if (result) result->QuadPart = position_;
    return S_OK;
  }
  STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
  STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP Commit(DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP Revert() override { return E_NOTIMPL; }
  STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
    if (statFails) return E_NOTIMPL;
    *stat = {};
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = size;
    return S_OK;
  }
  STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

 private:
  LONG refs_ = 1;
  ULONGLONG position_ = 0;
};

const HandlerHost& Host() { return HandlerEnvironment::Host(); }

// Owns one handler object.
struct Provider {
  IInitializeWithStream* init = nullptr;
  IThumbnailProvider* thumbnails = nullptr;
  Provider() {
    EXPECT_EQ(Host().CreateProvider(&init), S_OK);
    if (init) {
      EXPECT_EQ(init->QueryInterface(pdft::kThumbSlotIid, reinterpret_cast<void**>(&thumbnails)),
                S_OK);
    }
  }
  ~Provider() {
    if (thumbnails) thumbnails->Release();
    if (init) init->Release();
  }
  Provider(const Provider&) = delete;
  Provider& operator=(const Provider&) = delete;
};

HRESULT GetThumbnail(IThumbnailProvider* provider, UINT cx) {
  HBITMAP bitmap = nullptr;
  WTS_ALPHATYPE alpha = WTSAT_ARGB;
  const HRESULT hr = provider->GetThumbnail(cx, &bitmap, &alpha);
  if (bitmap) DeleteObject(bitmap);
  if (FAILED(hr)) {
    EXPECT_EQ(bitmap, nullptr);
    EXPECT_EQ(alpha, WTSAT_UNKNOWN);
  }
  return hr;
}

TEST(HandlerCom, ClassFactory) {
  IClassFactory* factory = nullptr;
  EXPECT_EQ(Host().GetClassObject(pdft::kHandlerClsid, IID_IClassFactory, nullptr), E_POINTER);
  EXPECT_EQ(
      Host().GetClassObject(IID_IUnknown, IID_IClassFactory, reinterpret_cast<void**>(&factory)),
      CLASS_E_CLASSNOTAVAILABLE);
  EXPECT_EQ(factory, nullptr);
  IUnknown* object = nullptr;
  IUnknown* outer = reinterpret_cast<IUnknown*>(&factory);  // never called
  EXPECT_EQ(
      Host().factory()->CreateInstance(outer, IID_IUnknown, reinterpret_cast<void**>(&object)),
      CLASS_E_NOAGGREGATION);
  EXPECT_EQ(Host().factory()->CreateInstance(nullptr, IID_IUnknown, nullptr), E_POINTER);
  EXPECT_EQ(Host().factory()->CreateInstance(nullptr, IID_IPersistStream,
                                             reinterpret_cast<void**>(&object)),
            E_NOINTERFACE);
  EXPECT_EQ(object, nullptr);
}

TEST(HandlerCom, QueryInterface) {
  Provider p;
  ASSERT_NE(p.thumbnails, nullptr);
  IUnknown* unknown = nullptr;
  EXPECT_EQ(p.init->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unknown)), S_OK);
  if (unknown) unknown->Release();
  IInitializeWithStream* init = nullptr;
  EXPECT_EQ(
      p.thumbnails->QueryInterface(pdft::kIidInitializeWithStream, reinterpret_cast<void**>(&init)),
      S_OK);
  if (init) init->Release();
  IPersistStream* persist = nullptr;
  EXPECT_EQ(p.init->QueryInterface(IID_IPersistStream, reinterpret_cast<void**>(&persist)),
            E_NOINTERFACE);
  EXPECT_EQ(persist, nullptr);
  // Stream only: Explorer must never hand it a path.
  IInitializeWithFile* withFile = nullptr;
  EXPECT_EQ(p.init->QueryInterface(IID_IInitializeWithFile, reinterpret_cast<void**>(&withFile)),
            E_NOINTERFACE);
  EXPECT_EQ(p.init->QueryInterface(IID_IUnknown, nullptr), E_POINTER);
}

TEST(HandlerCom, CanUnloadOnlyWithoutObjectsAndLocks) {
  {
    Provider p;
    EXPECT_EQ(Host().CanUnloadNow(), S_FALSE);
  }
  EXPECT_EQ(Host().CanUnloadNow(), S_OK);
  EXPECT_EQ(Host().factory()->LockServer(TRUE), S_OK);
  EXPECT_EQ(Host().CanUnloadNow(), S_FALSE);
  EXPECT_EQ(Host().factory()->LockServer(FALSE), S_OK);
  EXPECT_EQ(Host().CanUnloadNow(), S_OK);
}

TEST(RenderErrors, NullOutParameters) {
  FakeStream stream;
  stream.size = 1000;
  Provider p;
  ASSERT_EQ(p.init->Initialize(&stream, STGM_READ), S_OK);
  HBITMAP bitmap = nullptr;
  WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
  EXPECT_EQ(p.thumbnails->GetThumbnail(256, nullptr, &alpha), E_POINTER);
  EXPECT_EQ(p.thumbnails->GetThumbnail(256, &bitmap, nullptr), E_POINTER);
}

TEST(RenderErrors, InitializeArguments) {
  Provider p;
  EXPECT_EQ(p.init->Initialize(nullptr, STGM_READ), E_INVALIDARG);
  FakeStream first, second;
  EXPECT_EQ(p.init->Initialize(&first, STGM_READ), S_OK);
  EXPECT_EQ(p.init->Initialize(&second, STGM_READ), HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED));
  EXPECT_EQ(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), static_cast<HRESULT>(0x800704DF));
  EXPECT_EQ(second.refs(), 1) << "the second stream must not be kept";
}

TEST(RenderErrors, ReleasesTheStream) {
  FakeStream stream;
  {
    Provider p;
    ASSERT_EQ(p.init->Initialize(&stream, STGM_READ), S_OK);
    EXPECT_GT(stream.refs(), 1);
  }
  EXPECT_EQ(stream.refs(), 1);
}

TEST(RenderErrors, GetThumbnailBeforeInitialize) {
  Provider p;
  EXPECT_EQ(GetThumbnail(p.thumbnails, 256), E_UNEXPECTED);
}

TEST(RenderErrors, ZeroEdge) {
  FakeStream stream;
  stream.size = 1000;
  Provider p;
  ASSERT_EQ(p.init->Initialize(&stream, STGM_READ), S_OK);
  EXPECT_EQ(GetThumbnail(p.thumbnails, 0), E_INVALIDARG);
}

TEST(RenderErrors, EmptyStream) {
  IStream* stream = SHCreateMemStream(nullptr, 0);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(HresultText(Host().RenderStream(stream, 256).hr), HresultText(WTS_E_FAILEDEXTRACTION));
  stream->Release();
}

TEST(RenderErrors, FilesOver256MiBKeepTheIcon) {
  FakeStream stream;
  stream.size = (256ull << 20) + 1;
  EXPECT_EQ(HresultText(Host().RenderStream(&stream, 256).hr), HresultText(WTS_E_FAILEDEXTRACTION));
}

TEST(RenderErrors, StatAndSeekBothFail) {
  FakeStream stream;
  stream.size = 1000;
  stream.statFails = true;
  stream.seekFails = true;
  EXPECT_EQ(HresultText(Host().RenderStream(&stream, 256).hr), HresultText(WTS_E_FAILEDEXTRACTION));
}

TEST(RenderErrors, SizeFromSeekWhenStatFails) {
  FakeStream stream;
  stream.size = 1000;  // spaces: not a PDF, so PDFium refuses it after the read
  stream.statFails = true;
  EXPECT_EQ(HresultText(Host().RenderStream(&stream, 256).hr), HresultText(WTS_E_FAILEDEXTRACTION));
}

TEST(RenderErrors, ReadFailsOrReturnsNothing) {
  FakeStream failing;
  failing.size = 1000;
  failing.readFails = true;
  EXPECT_EQ(HresultText(Host().RenderStream(&failing, 256).hr),
            HresultText(WTS_E_FAILEDEXTRACTION));
  FakeStream empty;
  empty.size = 1000;
  empty.readsNothing = true;
  EXPECT_EQ(HresultText(Host().RenderStream(&empty, 256).hr), HresultText(WTS_E_FAILEDEXTRACTION));
}

TEST(RenderErrors, RendersFromAMemoryStream) {
  // The same bytes as 01-portrait.pdf, through an in-memory stream instead of a file.
  HANDLE file = CreateFileW(HandlerEnvironment::Pdf(L"01-").c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  BYTE data[4096];
  DWORD read = 0;
  ReadFile(file, data, sizeof(data), &read, nullptr);
  CloseHandle(file);
  IStream* stream = SHCreateMemStream(data, read);
  ASSERT_NE(stream, nullptr);
  const Rendered r = Host().RenderStream(stream, 256);
  stream->Release();
  EXPECT_EQ(r.hr, S_OK);
  EXPECT_EQ(r.width, 181);
  EXPECT_EQ(r.height, 256);
}

}  // namespace
}  // namespace pdft::test
