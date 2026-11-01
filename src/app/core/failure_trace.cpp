#include "failure_trace.h"

#include <algorithm>

#include "strings.h"

namespace pdft {
namespace {

// The calling thread's active trace. A constant initializer: no code runs before main
// (startup.h).
thread_local FailureTrace* t_active = nullptr;

// ASCII source text (file and function names) as UTF-16.
std::wstring Widen(const char* text) {
  std::wstring wide;
  for (const char* c = text; c && *c; ++c) wide.push_back(static_cast<unsigned char>(*c));
  return wide;
}

}  // namespace

FailureTrace::FailureTrace() noexcept : previous_(t_active) { t_active = this; }

FailureTrace::~FailureTrace() { t_active = previous_; }

TraceStep::TraceStep(const wchar_t* name) noexcept : trace_(t_active) {
  if (!trace_) return;
  previous_ = trace_->step_;
  trace_->step_ = name;
  trace_->failure_.reset();  // the step went on: whatever was pending has been handled
}

TraceStep::~TraceStep() {
  if (trace_) trace_->step_ = previous_;
}

void TraceStep::Set(const wchar_t* name) noexcept {
  if (!trace_) return;
  trace_->step_ = name;
  trace_->failure_.reset();  // the step went on: whatever was pending has been handled
}

HRESULT NoteFailure(const SourceSite& site, HRESULT hr, std::wstring_view detail) noexcept {
  FailureTrace* trace = t_active;
  if (!trace) return hr;
  if (SUCCEEDED(hr)) {
    trace->failure_.reset();  // whatever was pending has been handled
    return hr;
  }
  if (trace->failure_) return hr;  // a caller passing the pending failure on
  TracedFailure& failure = trace->failure_.emplace();
  failure.site = site;
  failure.hr = hr;
  failure.step = trace->step_ ? trace->step_ : L"";
  const size_t count = std::min(detail.size(), kTraceDetailChars);
  if (count > 0) std::copy_n(detail.data(), count, failure.detail.data());
  failure.detail[count] = L'\0';
  return hr;
}

void NoteHandled() noexcept {
  if (t_active) t_active->failure_.reset();
}

std::wstring TracedFailureText(const TracedFailure& failure) {
  std::wstring text;
  if (failure.step && *failure.step) text = L"in " + std::wstring(failure.step) + L" ";
  text += L"at " + Widen(failure.site.file) + L":" + std::to_wstring(failure.site.line) + L" " +
          Widen(failure.site.function) + L" hr=" + HResultText(failure.hr);
  if (failure.detail[0] != L'\0') text += L" (" + std::wstring(failure.detail.data()) + L")";
  return text;
}

}  // namespace pdft
