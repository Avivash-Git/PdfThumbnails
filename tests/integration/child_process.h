// Starting programs from the integration tests with their standard handles on pipes.

#pragma once

#include <windows.h>

#include <string>

namespace pdft::test {

// This test program's full path, and its folder (where the app's executables are too, in the
// build folder and in the test bundle).
std::wstring SelfPath();
std::wstring SelfFolder();

// The suffix main() gives this run's named objects (SetObjectNameSuffixForTesting in
// instance.h). A child that must share them gets it on its command line.
std::wstring& NamedObjectSuffix();

struct CapturedRun {
  HRESULT hr = E_FAIL;  // starting the program and waiting for it
  DWORD exitCode = 0;
  std::string out;  // what it wrote to standard output, as bytes
  std::string err;  // standard error
};

// Runs |exe| with |arguments| (already quoted), standard output and error each into a pipe, no
// standard input and no console window, and waits up to |timeoutMs|; after that it is ended and
// hr is HRESULT_FROM_WIN32(ERROR_TIMEOUT). The child inherits these two pipe ends and no other
// handle.
CapturedRun RunCaptured(const std::wstring& exe, const std::wstring& arguments, DWORD timeoutMs);

// A child whose standard input and output are pipes held by this process (standard error goes
// to the same pipe as standard output).
class PipedChild {
 public:
  PipedChild() = default;
  PipedChild(const PipedChild&) = delete;
  PipedChild& operator=(const PipedChild&) = delete;
  // Closes its input and, if it has not ended within 10 s, ends it.
  ~PipedChild();

  // With |token| (a primary token), the child runs as that token instead of this process's.
  HRESULT Start(const std::wstring& exe, const std::wstring& arguments, HANDLE token = nullptr);
  // The next line of its output, without the line end; empty when the output ended first.
  std::string ReadLine();
  // Closes its standard input: it reads end of file.
  void CloseInput();
  // Waits for it to end and returns its exit code; MAXDWORD on timeout.
  DWORD Wait(DWORD timeoutMs);

 private:
  HANDLE process_ = nullptr;
  HANDLE input_ = nullptr;   // write end of its standard input
  HANDLE output_ = nullptr;  // read end of its standard output
};

}  // namespace pdft::test
