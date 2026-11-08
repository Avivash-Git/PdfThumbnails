// Starting one of the app's own programs as a child of the settings window and reading what it
// says: the rebuild, the uninstall, the install and the preview all run this way.
//
// The child inherits exactly two handles: one pipe for its standard output and error, and NUL for
// its input. It gets no console and is put in no job: a File Explorer it starts must not end up
// inside a job of this window's.

#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pdft {

struct ProgramRequest {
  std::filesystem::path program;  // a full path, never searched for
  std::wstring arguments;         // after the program's own name on the command line
  std::filesystem::path folder;   // its current folder; empty: this process's
};

class RunningProgram {
 public:
  [[nodiscard]] static HRESULT Start(const ProgramRequest& request,
                                     std::unique_ptr<RunningProgram>* program) noexcept;

  HANDLE process() const noexcept { return process_.get(); }
  // The read end of its output: reading ends when it, and whatever it left the pipe to, are gone.
  HANDLE output() const noexcept { return output_.get(); }

  // Ends it, if it still runs. Only for a program whose work may be thrown away.
  void Terminate() noexcept;

  // Waits at most |timeoutMs| for it to end. S_OK with its exit code; HRESULT_FROM_WIN32(
  // ERROR_TIMEOUT) when it still runs.
  [[nodiscard]] HRESULT WaitForExit(DWORD timeoutMs, DWORD* exitCode) noexcept;

 private:
  RunningProgram() = default;

  wil::unique_handle process_;
  wil::unique_hfile output_;
};

// Splits a program's UTF-8 output into lines as it arrives (pure): CR LF or LF ends a line,
// and an invalid byte becomes U+FFFD. May throw std::bad_alloc.
class OutputLines {
 public:
  using LineFn = std::function<void(std::wstring_view)>;
  void Feed(std::string_view bytes, const LineFn& onLine);
  // The last line, when the output did not end with a line end.
  void Finish(const LineFn& onLine);

 private:
  std::string partial_;
};

// Reads |output| until the program has closed it, so the program never blocks on a full pipe.
// Keeps at most |cap| bytes in *kept (*truncated says whether more came) and, when |onLine| is
// not empty, hands over every line as it arrives, also those past |cap|. S_OK at the end of the
// output; a failure when reading fails otherwise.
[[nodiscard]] HRESULT ReadProgramOutput(HANDLE output, size_t cap,
                                        const OutputLines::LineFn& onLine, std::string* kept,
                                        bool* truncated) noexcept;

}  // namespace pdft
