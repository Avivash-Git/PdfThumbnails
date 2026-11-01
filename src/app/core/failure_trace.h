// Where an administrator step's failure started, for the report it sends the process that asked.
//
// The elevated process writes no log: the lines it sends back are all anyone sees of it (in the
// asking process's log and output, and in a CI job's log). A step that ends with nothing but an
// HRESULT leaves every call on its way a suspect. So the functions on the way of the
// administrator steps pass what they return through PDFT_TRACE, and the step's report names the
// failure that ended it: the source file's name, the line, the function, the HRESULT, the part
// of the step it happened in (TraceStep) and, where it helps, which object (a binary's name, how
// many folders up).
//
// Never a path: the file name is cut from __FILE__ at compile time, so the binary holds
// "install.cpp" and not the build machine's folder (gate.hardening.* would find that), and a
// detail is a count or the name (never the path) of one file or folder: one of the product's own,
// a build folder's, or one under a build's notice folder ("licenses"). The last come from the
// build folder being installed, so its owner chooses them: at most kTraceDetailChars characters
// of them reach the report, with control characters replaced (ElevatedReporter::Line), and the
// report goes to that same user's process.
//
// What a trace keeps is the place where the failure being passed on started:
//   - a failure noted while none is pending starts one: the place, the HRESULT, the part of the
//     step and the detail are kept;
//   - a failure noted while one is pending is a caller passing it on (with the same or another
//     HRESULT): nothing changes, so the innermost place stays;
//   - a success noted means that the pending failure was handled (a retry, a fallback): it is
//     dropped. So does NoteHandled() (a failure tried again or tolerated without a success noted
//     in between), and so does a part of the step that starts (TraceStep): the step went on.
// Notes go to the trace that is active on the calling thread. Without one (every process but the
// elevated one, and most tests) PDFT_TRACE only returns its value.

#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace pdft {

// A place in the source.
struct SourceSite {
  const char* file = "";  // the file's name, without its folder
  unsigned line = 0;
  const char* function = "";  // the function's own name (__func__)
};

// The longest detail kept, in characters; a longer one is cut.
inline constexpr size_t kTraceDetailChars = 47;

// The failure a trace keeps.
struct TracedFailure {
  SourceSite site;
  HRESULT hr = S_OK;
  const wchar_t* step = L"";                            // TraceStep's name; "" when none
  std::array<wchar_t, kTraceDetailChars + 1> detail{};  // null-terminated; empty when none
};

// Notes |hr| at |site|, with |detail| (cut to kTraceDetailChars), in the calling thread's active
// trace as the header comment says, and returns |hr|. Use PDFT_TRACE, which fills in |site|.
HRESULT NoteFailure(const SourceSite& site, HRESULT hr, std::wstring_view detail = {}) noexcept;

// The pending failure in the calling thread's active trace was handled (it is tried again, or
// tolerated and listed): drops it, so that a later failure is kept where it starts and not taken
// for this one passed on.
void NoteHandled() noexcept;

// While it exists, the calling thread's active trace: what PDFT_TRACE notes on this thread goes
// here. A trace made while another is active hides it until it goes away.
class FailureTrace {
 public:
  FailureTrace() noexcept;
  ~FailureTrace();
  FailureTrace(const FailureTrace&) = delete;
  FailureTrace& operator=(const FailureTrace&) = delete;

  // The pending failure: where the last failure noted, and not handled since, started.
  const std::optional<TracedFailure>& failure() const noexcept { return failure_; }

 private:
  friend HRESULT NoteFailure(const SourceSite& site, HRESULT hr, std::wstring_view detail) noexcept;
  friend void NoteHandled() noexcept;
  friend class TraceStep;

  FailureTrace* previous_ = nullptr;
  const wchar_t* step_ = L"";
  std::optional<TracedFailure> failure_;
};

// Names the part of the step that the notes after it belong to, in the calling thread's active
// trace (none: does nothing), until Set() names another or this object goes away, when the name
// before it comes back. Names must outlive the trace: string literals. A part that starts (here or
// in Set) drops the pending failure: the step went on, so it was handled.
class TraceStep {
 public:
  explicit TraceStep(const wchar_t* name) noexcept;
  ~TraceStep();
  TraceStep(const TraceStep&) = delete;
  TraceStep& operator=(const TraceStep&) = delete;

  void Set(const wchar_t* name) noexcept;

 private:
  FailureTrace* trace_ = nullptr;
  const wchar_t* previous_ = L"";
};

// "in copy-build at install.cpp:47 MakeRealFolder hr=0x80070005 (PdfThumbnailsCli.exe)": the
// part of the step only when one was named, the detail only when there is one. May throw
// std::bad_alloc.
std::wstring TracedFailureText(const TracedFailure& failure);

namespace details {

// Where the file name starts in |path|: after its last \ or /.
consteval size_t FileNameStart(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string_view::npos ? 0 : slash + 1;
}

// |path|'s file name, null-terminated, in an array of exactly that size. Only ever evaluated at
// compile time, so |path| itself never reaches the binary.
template <size_t Size>
consteval std::array<char, Size> CutFileName(std::string_view path) {
  std::array<char, Size> name{};
  const std::string_view cut = path.substr(FileNameStart(path));
  for (size_t i = 0; i + 1 < Size && i < cut.size(); ++i) name[i] = cut[i];
  return name;
}

}  // namespace details
}  // namespace pdft

// This place in the source: the file's name (cut from __FILE__ at compile time), the line and the
// function.
#define PDFT_SOURCE_SITE                                                                          \
  (::pdft::SourceSite{                                                                            \
      []() noexcept {                                                                             \
        static constexpr auto kName =                                                             \
            ::pdft::details::CutFileName<std::string_view(__FILE__).size() -                      \
                                         ::pdft::details::FileNameStart(__FILE__) + 1>(__FILE__); \
        return kName.data();                                                                      \
      }(),                                                                                        \
      static_cast<unsigned>(__LINE__), __func__})

// The HRESULT |expr|, noted here in the calling thread's active trace; its value.
#define PDFT_TRACE(expr) ::pdft::NoteFailure(PDFT_SOURCE_SITE, (expr))

// The same with |detail|: which object (a count, or one file's or folder's name as the header
// comment says), never a path.
#define PDFT_TRACE_DETAIL(expr, detail) ::pdft::NoteFailure(PDFT_SOURCE_SITE, (expr), (detail))
