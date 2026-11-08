#include "gui_driver.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "project_config.h"

namespace pdft::test {
namespace {

std::wstring SelfFolder() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring self(path, length);
  return self.substr(0, self.find_last_of(L'\\'));
}

struct WindowSearch {
  DWORD pid;
  const wchar_t* className;
  HWND found;
};

BOOL CALLBACK CheckWindow(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<WindowSearch*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  wchar_t name[128] = {};
  GetClassNameW(window, name, ARRAYSIZE(name));
  if (pid == search->pid && wcscmp(name, search->className) == 0) {
    search->found = window;
    return FALSE;
  }
  return TRUE;
}

uint32_t Crc32(const std::string& data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::string BigEndian(uint32_t value) {
  return {static_cast<char>(value >> 24), static_cast<char>(value >> 16),
          static_cast<char>(value >> 8), static_cast<char>(value)};
}

std::string Chunk(const char* tag, const std::string& data) {
  const std::string body = tag + data;
  return BigEndian(static_cast<uint32_t>(data.size())) + body + BigEndian(Crc32(body));
}

// Zlib's format without compression: blocks of at most 65535 bytes stored as they are.
std::string Stored(const std::string& raw) {
  std::string out = "\x78\x01";
  uint32_t a = 1;
  uint32_t b = 0;
  for (const unsigned char byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t at = 0; at < raw.size();) {
    const size_t size = raw.size() - at < 65535 ? raw.size() - at : 65535;
    out += static_cast<char>(at + size == raw.size() ? 1 : 0);
    out += static_cast<char>(size);
    out += static_cast<char>(size >> 8);
    out += static_cast<char>(~size);
    out += static_cast<char>(~size >> 8);
    out.append(raw, at, size);
    at += size;
  }
  return out + BigEndian((b << 16) | a);
}

}  // namespace

bool WaitFor(const std::function<bool()>& condition, DWORD timeoutMs) {
  const ULONGLONG start = GetTickCount64();
  for (;;) {
    if (condition()) return true;
    if (GetTickCount64() - start >= timeoutMs) return false;
    Sleep(50);
  }
}

HWND TopLevelWindowOf(DWORD pid, const wchar_t* className) {
  WindowSearch search{pid, className, nullptr};
  EnumWindows(CheckWindow, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

bool Program::Start(const std::wstring& arguments) {
  const std::wstring exe = SelfFolder() + L"\\" + kSettingsExe;
  std::wstring commandLine = L"\"" + exe + L"\" " + arguments;
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION started = {};
  if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                      &startup, &started)) {
    return false;
  }
  CloseHandle(started.hThread);
  process_.reset(started.hProcess);
  id_ = started.dwProcessId;
  return true;
}

bool Program::Running() const noexcept {
  return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
}

DWORD Program::WaitForExit(DWORD timeoutMs) noexcept {
  DWORD code = 0xFFFFFFFFu;
  if (process_ && WaitForSingleObject(process_.get(), timeoutMs) == WAIT_OBJECT_0) {
    GetExitCodeProcess(process_.get(), &code);
  }
  return code;
}

void Program::Kill() noexcept {
  if (!Running()) return;
  TerminateProcess(process_.get(), 1);
  WaitForSingleObject(process_.get(), 5000);
}

std::wstring ControlText(HWND dialog, int id) {
  // WM_GETTEXT itself: GetDlgItemText and GetWindowText only see the caption Windows keeps for a
  // window of another process, which an edit box does not have.
  wchar_t text[1024] = {};
  const LRESULT length = SendMessageW(GetDlgItem(dialog, id), WM_GETTEXT, ARRAYSIZE(text),
                                      reinterpret_cast<LPARAM>(text));
  return std::wstring(text, static_cast<size_t>(length));
}

bool SaveWindowPicture(HWND window, const std::wstring& file) {
  RECT bounds = {};
  if (!GetWindowRect(window, &bounds)) return false;
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  if (width <= 0 || height <= 0) return false;
  BITMAPINFO format = {};
  format.bmiHeader.biSize = sizeof(format.bmiHeader);
  format.bmiHeader.biWidth = width;
  format.bmiHeader.biHeight = -height;
  format.bmiHeader.biPlanes = 1;
  format.bmiHeader.biBitCount = 32;
  format.bmiHeader.biCompression = BI_RGB;
  wil::unique_hdc memory(CreateCompatibleDC(nullptr));
  void* bits = nullptr;
  wil::unique_hbitmap bitmap(
      CreateDIBSection(memory.get(), &format, DIB_RGB_COLORS, &bits, nullptr, 0));
  if (!bitmap) return false;
  const HGDIOBJ old = SelectObject(memory.get(), bitmap.get());
  const BOOL painted = PrintWindow(window, memory.get(), PW_RENDERFULLCONTENT);
  SelectObject(memory.get(), old);
  if (!painted) return false;

  std::string raw;
  raw.reserve((size_t{static_cast<UINT>(width)} * 3 + 1) * static_cast<UINT>(height));
  const auto* pixels = static_cast<const uint8_t*>(bits);
  for (int y = 0; y < height; ++y) {
    raw += '\0';
    for (int x = 0; x < width; ++x) {
      const uint8_t* pixel = pixels + (size_t{static_cast<UINT>(y)} * width + x) * 4;
      raw += static_cast<char>(pixel[2]);
      raw += static_cast<char>(pixel[1]);
      raw += static_cast<char>(pixel[0]);
    }
  }
  std::string header =
      BigEndian(static_cast<uint32_t>(width)) + BigEndian(static_cast<uint32_t>(height));
  header += std::string("\x08\x02\x00\x00\x00", 5);
  std::ofstream out(file, std::ios::binary);
  out << "\x89PNG\r\n\x1a\n"
      << Chunk("IHDR", header) << Chunk("IDAT", Stored(raw)) << Chunk("IEND", "");
  return out.good();
}

}  // namespace pdft::test
