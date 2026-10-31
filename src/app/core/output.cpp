#include "output.h"

#include <climits>

#include "strings.h"

namespace pdft {
namespace {

constexpr std::wstring_view kRed = L"\x1b[31m";
constexpr std::wstring_view kReset = L"\x1b[0m";

// NO_COLOR set to anything but an empty string (https://no-color.org).
bool NoColorSet() noexcept {
  wchar_t value[2] = {};
  return GetEnvironmentVariableW(L"NO_COLOR", value, 2) > 0;
}

// |text| with every line ending in CR LF, the last one included.
std::wstring WithCrLf(std::wstring_view text) {
  std::wstring result;
  result.reserve(text.size() + 2);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') continue;
    if (text[i] == L'\n') {
      result += L"\r\n";
    } else {
      result += text[i];
    }
  }
  result += L"\r\n";
  return result;
}

}  // namespace

GuiOutputSource ChooseGuiOutput(DWORD stdOutFileType) noexcept {
  return stdOutFileType == FILE_TYPE_UNKNOWN ? GuiOutputSource::kParentConsole
                                             : GuiOutputSource::kInherited;
}

bool ShouldUseColor(bool isConsole, bool virtualTerminal, bool noColor) noexcept {
  return isConsole && virtualTerminal && !noColor;
}

std::string EncodeLine(std::wstring_view text) { return WideToUtf8Lossy(WithCrLf(text)); }

std::wstring Red(std::wstring_view text) {
  std::wstring result(kRed);
  result += text;
  result += kReset;
  return result;
}

Output::Output(Mode mode) noexcept {
  if (mode == Mode::kCli) {
    OpenStandardStreams(true);
  } else if (ChooseGuiOutput(GetFileType(GetStdHandle(STD_OUTPUT_HANDLE))) ==
             GuiOutputSource::kInherited) {
    OpenStandardStreams(false);
  } else {
    // Attaching ties this process to the terminal (closing it would end the process), so it
    // waits until there is something to print.
    attachPending_ = true;
  }
}

Output::Output(HANDLE out, HANDLE err) noexcept {
  out_.handle = out;
  err_.handle = err;
}

Output::~Output() {
  for (Stream* stream : {&out_, &err_}) {
    if (stream->restoreMode) SetConsoleMode(stream->handle, stream->originalMode);
  }
}

void Output::OpenStandardStreams(bool allowColor) noexcept {
  const bool noColor = NoColorSet();
  const struct {
    Stream* stream;
    DWORD id;
  } streams[] = {{&out_, STD_OUTPUT_HANDLE}, {&err_, STD_ERROR_HANDLE}};
  for (const auto& [stream, id] : streams) {
    HANDLE handle = GetStdHandle(id);
    if (!handle || handle == INVALID_HANDLE_VALUE || GetFileType(handle) == FILE_TYPE_UNKNOWN) {
      continue;  // closed or never given: this stream goes nowhere
    }
    stream->handle = handle;
    DWORD mode = 0;
    stream->console = GetConsoleMode(handle, &mode) != FALSE;
    bool virtualTerminal = false;
    if (stream->console && allowColor && !noColor) {
      if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
        virtualTerminal = true;
      } else if (SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        virtualTerminal = true;
        stream->originalMode = mode;
        stream->restoreMode = true;
      }
    }
    stream->color = ShouldUseColor(stream->console, virtualTerminal, noColor);
  }
}

void Output::AttachParentConsole() noexcept {
  attachPending_ = false;
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  console_.reset(CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                             nullptr));
  if (!console_) return;
  for (Stream* stream : {&out_, &err_}) {
    stream->handle = console_.get();
    stream->console = true;
  }
}

void Output::Write(Stream& stream, std::wstring_view text, bool error) noexcept {
  if (attachPending_) AttachParentConsole();
  if (!stream.handle) return;
  try {
    DWORD written = 0;
    if (stream.console) {
      const std::wstring line = WithCrLf(error && stream.color ? Red(text) : std::wstring(text));
      if (line.size() > INT_MAX) return;
      WriteConsoleW(stream.handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    } else {
      const std::string bytes = EncodeLine(text);
      if (bytes.size() > INT_MAX) return;
      WriteFile(stream.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }
  } catch (...) {
    // Out of memory: the line is lost, the command goes on.
  }
}

void Output::Line(std::wstring_view text) noexcept { Write(out_, text, false); }

void Output::Error(std::wstring_view text) noexcept { Write(err_, text, true); }

}  // namespace pdft
