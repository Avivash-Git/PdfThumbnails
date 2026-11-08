// Driving the real settings window from another process: starting it, finding its window, and
// keeping a picture of it for a person to look at.

#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <functional>
#include <string>

namespace pdft::test {

// Calls |condition| every 50 ms until it is true or |timeoutMs| has passed.
bool WaitFor(const std::function<bool()>& condition, DWORD timeoutMs);

// A top-level window of process |pid| with the class |className|; nullptr when there is none.
HWND TopLevelWindowOf(DWORD pid, const wchar_t* className);

// The settings executable next to this program.
class Program {
 public:
  Program() = default;
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;
  ~Program() { Kill(); }

  bool Start(const std::wstring& arguments);
  DWORD id() const noexcept { return id_; }
  bool Running() const noexcept;
  // Its exit code; 0xFFFFFFFF when it did not end within |timeoutMs|.
  DWORD WaitForExit(DWORD timeoutMs) noexcept;
  void Kill() noexcept;

 private:
  wil::unique_process_handle process_;
  DWORD id_ = 0;
};

// The text of a control of another process.
std::wstring ControlText(HWND dialog, int id);

// Saves the whole window as a PNG. True when it was written.
bool SaveWindowPicture(HWND window, const std::wstring& file);

}  // namespace pdft::test
