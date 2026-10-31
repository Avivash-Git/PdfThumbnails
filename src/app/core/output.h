// What a command prints, and where it goes.
//
// Command-line executable: standard output and error. On a console through WriteConsoleW (any
// character shows, whatever the console code page); redirected to a pipe or a file as UTF-8
// without a byte order mark. Errors are red on a console that understands VT sequences, unless
// NO_COLOR is set.
//
// Settings executable (no console of its own), in this order:
//   1. the standard handles it inherited, when standard output is a pipe, file or console
//      (a script started it with redirection);
//   2. otherwise the console of the process that started it (a terminal), attached on the first
//      line written, best effort: the terminal has usually printed its prompt already;
//   3. otherwise nowhere (started from Explorer or Task Scheduler): the log records what ran.

#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <string>
#include <string_view>

#include "mode.h"

namespace pdft {

// Where the settings executable writes (pure): its inherited handles, or the parent's console.
enum class GuiOutputSource { kInherited, kParentConsole };
GuiOutputSource ChooseGuiOutput(DWORD stdOutFileType) noexcept;

// Whether a stream gets VT colour (pure): only a console that accepted
// ENABLE_VIRTUAL_TERMINAL_PROCESSING, and only without NO_COLOR (https://no-color.org: set to
// anything but an empty string).
bool ShouldUseColor(bool isConsole, bool virtualTerminal, bool noColor) noexcept;

// One line as written to a pipe or file (pure): UTF-8 without a byte order mark, CR LF at the
// end. May throw std::bad_alloc.
std::string EncodeLine(std::wstring_view text);

// |text| in red for a VT console (pure). May throw std::bad_alloc.
std::wstring Red(std::wstring_view text);

class Output {
 public:
  // The output of the executable running in |mode| (see the top of this file).
  static Output ForMode(Mode mode) noexcept { return Output(mode); }
  // Writes to |out| and |err| as to a pipe or file (no colour); for tests. Not owned.
  static Output ForHandles(HANDLE out, HANDLE err) noexcept { return Output(out, err); }

  Output(const Output&) = delete;
  Output& operator=(const Output&) = delete;
  // Puts back the console modes it changed.
  ~Output();

  // A line on standard output. Multi-line text is fine: each line ends in CR LF on a pipe.
  void Line(std::wstring_view text) noexcept;
  // A line on standard error.
  void Error(std::wstring_view text) noexcept;

 private:
  struct Stream {
    HANDLE handle = nullptr;
    bool console = false;
    bool color = false;
    DWORD originalMode = 0;  // restored when |color| switched VT processing on
    bool restoreMode = false;
  };

  explicit Output(Mode mode) noexcept;
  Output(HANDLE out, HANDLE err) noexcept;

  void OpenStandardStreams(bool allowColor) noexcept;
  void AttachParentConsole() noexcept;
  void Write(Stream& stream, std::wstring_view text, bool error) noexcept;

  Stream out_;
  Stream err_;
  bool attachPending_ = false;  // settings executable: attach to the parent's console when needed
  wil::unique_hfile console_;   // CONOUT$ of the attached console
};

}  // namespace pdft
