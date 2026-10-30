// The test-PDF generator. MakeTestPdfs() reproduces the 1.x PowerShell generator
// (tools\make-test-pdfs.ps1, removed in 2.0) byte for byte, including the way .NET formatted
// and rounded its numbers, so results stay comparable with every earlier test run.

#include <windows.h>

#include "test_pdfs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pdft::test {
namespace {

std::string Int(long long value) { return std::to_string(value); }

// num/den (both >= 0, den > 0) with |decimals| decimals, rounded half away from zero: what .NET
// prints for "{0:0.00}" (it rounds the decimal digits, so 0.015 gives 0.02, unlike printf).
std::string Fixed(long long num, long long den, int decimals) {
  long long scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  const long long q = (2 * num * scale + den) / (2 * den);
  std::string out = Int(q / scale);
  if (decimals > 0) {
    const std::string fraction = Int(q % scale);
    out += '.';
    out.append(static_cast<size_t>(decimals) - fraction.size(), '0');
    out += fraction;
  }
  return out;
}

// PowerShell's [int] cast of a double: round half to even.
long long RoundHalfEven(double value) {
  const double floor = std::floor(value);
  const long long n = static_cast<long long>(floor);
  const double rest = value - floor;
  if (rest > 0.5) return n + 1;
  if (rest < 0.5) return n;
  return n % 2 == 0 ? n : n + 1;
}

std::string Join(const std::vector<std::string>& lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) out += '\n';
    out += lines[i];
  }
  return out;
}

// .NET Framework's System.Random (Knuth's subtractive generator), which the PowerShell
// generator used for 13-garbage.pdf with seed 1234.
class DotNetRandom {
 public:
  explicit DotNetRandom(int32_t seed) {
    constexpr int32_t kSeed = 161803398;
    const int32_t subtraction = seed == INT32_MIN ? INT32_MAX : (seed < 0 ? -seed : seed);
    int32_t mj = kSeed - subtraction;
    seeds_[55] = mj;
    int32_t mk = 1;
    for (int i = 1; i < 55; ++i) {
      const int ii = (21 * i) % 55;
      seeds_[ii] = mk;
      mk = mj - mk;
      if (mk < 0) mk += kBig;
      mj = seeds_[ii];
    }
    for (int k = 1; k < 5; ++k) {
      for (int i = 1; i < 56; ++i) {
        seeds_[i] -= seeds_[1 + (i + 30) % 55];
        if (seeds_[i] < 0) seeds_[i] += kBig;
      }
    }
  }

  void NextBytes(std::string* buffer) {
    for (char& c : *buffer) c = static_cast<char>(static_cast<uint8_t>(Sample() % 256));
  }

 private:
  static constexpr int32_t kBig = INT32_MAX;

  int32_t Sample() {
    if (++next_ >= 56) next_ = 1;
    if (++nextp_ >= 56) nextp_ = 1;
    int32_t value = seeds_[next_] - seeds_[nextp_];
    if (value == kBig) --value;
    if (value < 0) value += kBig;
    seeds_[next_] = value;
    return value;
  }

  int32_t seeds_[56] = {};
  int next_ = 0;
  int nextp_ = 21;
};

constexpr char kCatalog[] = "<< /Type /Catalog /Pages 2 0 R >>";
constexpr char kOnePageTree[] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
constexpr char kHelveticaBold[] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>";

// A test card: frame, red/green squares in the top corners, blue square bottom-left, a label.
std::string Card(int w, int h, std::string_view label, int labelY = -1) {
  if (labelY < 0) labelY = static_cast<int>(RoundHalfEven(h / 2.0));
  const int s = std::max(10, std::min(60, static_cast<int>(RoundHalfEven(std::min(w, h) / 8.0))));
  return Join({
      "0.2 g 3 w 10 10 " + Int(w - 20) + " " + Int(h - 20) + " re S",
      "1 0 0 rg 20 " + Int(h - 20 - s) + " " + Int(s) + " " + Int(s) + " re f",
      "0 0.6 0 rg " + Int(w - 20 - s) + " " + Int(h - 20 - s) + " " + Int(s) + " " + Int(s) +
          " re f",
      "0 0 1 rg 20 20 " + Int(s) + " " + Int(s) + " re f",
      "0 g BT /F1 36 Tf " + Int(RoundHalfEven(w * 0.1)) + " " + Int(labelY) + " Td (" +
          std::string(label) + ") Tj ET",
  });
}

// One-page document: |pageExtra| goes into the page dictionary, |more| are objects 6, 7, ...
std::string OnePager(std::string_view mediaBox, std::string_view content,
                     std::string_view pageExtra = {}, const std::vector<std::string>& more = {}) {
  std::vector<std::string> objects = {
      kCatalog,
      kOnePageTree,
      "<< /Type /Page /Parent 2 0 R /MediaBox [" + std::string(mediaBox) + "] " +
          std::string(pageExtra) + " /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      StreamObject(content),
      kHelveticaBold,
  };
  objects.insert(objects.end(), more.begin(), more.end());
  return SerializePdf(objects);
}

// XFA forms. Dynamic XFA (/NeedsRendering true) must keep the icon: its page 1 is only a
// placeholder. Static XFA renders from its ordinary page content like any other PDF.
std::string XfaPdf(std::string_view label, std::string_view catalogExtra) {
  return SerializePdf({
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [] /XFA 6 0 R >> " +
          std::string(catalogExtra) + " >>",
      kOnePageTree,
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> "
      ">> /Contents 4 0 R >>",
      StreamObject(Card(595, 842, label)),
      kHelveticaBold,
      StreamObject("<xdp:xdp xmlns:xdp=\"http://ns.adobe.com/xdp/\"><template "
                   "xmlns=\"http://www.xfa.org/schema/xfa-template/3.3/\"/></xdp:xdp>"),
  });
}

// 30 x 3000 pt strip of coloured bands: extreme aspect ratio.
std::string TallStripContent() {
  std::vector<std::string> bands;
  for (int i = 0; i <= 29; ++i) {
    bands.push_back(Fixed(i, 29, 2) + " 0.4 " + Fixed(29 - i, 29, 2) + " rg 0 " + Int(i * 100) +
                    " 30 100 re f");
  }
  return Join(bands);
}

// ~120,000 filled rectangles: a slow page for timing.
std::string HeavyContent() {
  std::string out;
  out.reserve(5'100'000);
  for (int y = 0; y < 400; ++y) {
    for (int x = 0; x < 300; ++x) {
      out += Fixed(x, 300, 2) + " " + Fixed(y, 400, 2) + " 0.5 rg " + Fixed(100 + 19 * x, 10, 1) +
             " " + Fixed(20 + 2 * y, 1, 1) + " 1.7 1.7 re f\n";
    }
  }
  return out;
}

// ---- Samples for the settings app (project-authored, MIT like the rest of the repo) ---------

// A4 portrait report page: header band with title, "greeked" text as grey bars, a bar chart,
// footer rule and page-number bar, so a crop at the top and bottom is easy to see.
std::string SampleDocumentContent() {
  std::vector<std::string> lines = {
      "0.13 0.36 0.66 rg 0 742 595 100 re f",
      "1 g BT /F1 30 Tf 48 784 Td (Sample Document) Tj ET",
      "0.62 0.76 0.92 rg 48 760 200 8 re f",
      "0.78 g",
  };
  const int widths[] = {499, 499, 470, 499, 330, 0, 499, 486, 499, 410};
  int y = 690;
  for (int width : widths) {
    if (width) lines.push_back("48 " + Int(y) + " " + Int(width) + " 7 re f");
    y -= 16;
  }
  lines.insert(lines.end(), {
                                "0.94 g 48 300 499 200 re f",
                                "0.13 0.36 0.66 rg 98 320 60 120 re f",
                                "0.95 0.55 0.15 rg 208 320 60 160 re f",
                                "0.20 0.62 0.35 rg 318 320 60 90 re f",
                                "0.80 0.25 0.25 rg 428 320 60 140 re f",
                                "0.4 G 1 w 68 320 m 527 320 l S",
                                "0.78 g",
                            });
  const int moreWidths[] = {499, 499, 455, 499, 260};
  y = 260;
  for (int width : moreWidths) {
    lines.push_back("48 " + Int(y) + " " + Int(width) + " 7 re f");
    y -= 16;
  }
  lines.insert(lines.end(), {
                                "0.6 G 1 w 48 70 m 547 70 l S",
                                "0.6 g 282 42 31 10 re f",
                            });
  return Join(lines);
}

// 16:9 slide: accent band on the left, title, bullets, a chart on the right, footer rule and
// page-number bar, so a crop at the sides is easy to see.
std::string SampleSlideContent() {
  std::vector<std::string> lines = {
      "0.97 g 0 0 960 540 re f",
      "0.13 0.36 0.66 rg 0 0 24 540 re f",
      "0.15 g BT /F1 44 Tf 72 448 Td (Sample Slide) Tj ET",
      "0.95 0.55 0.15 rg 72 426 180 6 re f",
  };
  const int widths[] = {320, 280, 340, 240};
  int y = 366;
  for (int width : widths) {
    lines.push_back("0.13 0.36 0.66 rg 72 " + Int(y) + " 12 12 re f");
    lines.push_back("0.72 g 100 " + Int(y) + " " + Int(width) + " 12 re f");
    y -= 48;
  }
  lines.insert(lines.end(), {
                                "1 g 520 120 380 260 re f",
                                "0.85 G 1 w 520 120 380 260 re S",
                                "0.13 0.36 0.66 rg 560 140 50 120 re f",
                                "0.95 0.55 0.15 rg 640 140 50 190 re f",
                                "0.20 0.62 0.35 rg 720 140 50 80 re f",
                                "0.80 0.25 0.25 rg 800 140 50 150 re f",
                                "0.4 G 1 w 540 140 m 880 140 l S",
                                "0.7 G 1 w 72 60 m 888 60 l S",
                                "0.6 g 864 34 24 10 re f",
                            });
  return Join(lines);
}

}  // namespace

std::string SerializePdf(const std::vector<std::string>& objects) {
  std::string out = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += Int(static_cast<long long>(i + 1)) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  const std::string count = Int(static_cast<long long>(objects.size() + 1));
  out += "xref\n0 " + count + "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    const std::string digits = Int(static_cast<long long>(offset));
    out += std::string(10 - std::min<size_t>(10, digits.size()), '0') + digits + " 00000 n \n";
  }
  out += "trailer\n<< /Size " + count + " /Root 1 0 R >>\nstartxref\n" +
         Int(static_cast<long long>(xref)) + "\n%%EOF\n";
  return out;
}

std::string StreamObject(std::string_view data, std::string_view dict) {
  return "<< " + std::string(dict) + " /Length " + Int(static_cast<long long>(data.size())) +
         " >>\nstream\n" + std::string(data) + "\nendstream";
}

std::vector<GeneratedFile> MakeTestPdfs() {
  const std::string portrait = OnePager("0 0 595 842", Card(595, 842, "PORTRAIT"));
  std::string garbage(16384, '\0');
  DotNetRandom(1234).NextBytes(&garbage);
  const size_t truncated =
      static_cast<size_t>(RoundHalfEven(static_cast<double>(portrait.size()) * 0.55)) + 1;

  return {
      {L"01-portrait.pdf", portrait},
      {L"02-landscape.pdf", OnePager("0 0 842 595", Card(842, 595, "LANDSCAPE"))},
      // /Rotate 90 turns the portrait page clockwise: shown landscape, red square top-right.
      {L"03-rotate90.pdf", OnePager("0 0 595 842", Card(595, 842, "ROTATE 90"), "/Rotate 90")},
      // Only the top half is visible: red and green squares, label, no blue square.
      {L"04-cropbox.pdf",
       OnePager("0 0 595 842", Card(595, 842, "CROPBOX", 600), "/CropBox [0 421 595 842]")},
      // Two pages; the thumbnail must show the green first page.
      {L"05-multipage.pdf",
       SerializePdf({
           kCatalog,
           "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>",
           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 "
           "R >> >> /Contents 4 0 R >>",
           StreamObject(
               "0 0.7 0 rg 0 0 595 842 re f\n1 g BT /F1 60 Tf 120 400 Td (PAGE ONE) Tj ET"),
           kHelveticaBold,
           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 "
           "R >> >> /Contents 7 0 R >>",
           StreamObject("1 0 0 rg 0 0 595 842 re f\n1 g BT /F1 60 Tf 120 400 Td (PAGE TWO) Tj ET"),
       })},
      {L"06-tall-strip.pdf", OnePager("0 0 30 3000", TallStripContent())},
      // A square annotation with its own appearance stream (needs FPDF_ANNOT to show up).
      {L"07-annotation.pdf",
       OnePager("0 0 595 842", Card(595, 842, "ANNOTATION", 700), "/Annots [6 0 R]",
                {
                    "<< /Type /Annot /Subtype /Square /Rect [150 150 450 450] /C [1 0.5 0] /F 4 "
                    "/AP << /N 7 0 R >> >>",
                    StreamObject("1 0.5 0 rg 0 0 300 300 re f",
                                 "/Type /XObject /Subtype /Form /BBox [0 0 300 300]"),
                })},
      {L"08-heavy.pdf", OnePager("0 0 595 842", HeavyContent())},
      {L"09-max-page-14400.pdf", OnePager("0 0 14400 14400", Card(14400, 14400, "HUGE PAGE"))},
      {L"10-zero-size-page.pdf", OnePager("0 0 0 0", "BT ET")},
      {L"11-no-pages.pdf", SerializePdf({kCatalog, "<< /Type /Pages /Kids [] /Count 0 >>"})},
      {L"12-empty.pdf", std::string()},
      {L"13-garbage.pdf", garbage},
      {L"14-text-file.pdf", "This is not a PDF.\n"},
      {L"15-truncated.pdf", portrait.substr(0, truncated)},
      // Non-ASCII file name (Greek and Japanese): Unicode paths through streams, shell items and
      // PNG names.
      {L"16-unicode-δοκιμή-テスト.pdf", portrait},
      {L"17-xfa-dynamic.pdf", XfaPdf("XFA DYNAMIC", "/NeedsRendering true")},
      {L"18-xfa-static.pdf", XfaPdf("XFA STATIC", "")},
  };
}

std::vector<GeneratedFile> MakeSamplePdfs() {
  return {
      {L"sample-document.pdf", OnePager("0 0 595 842", SampleDocumentContent())},
      {L"sample-slide.pdf", OnePager("0 0 960 540", SampleSlideContent())},
  };
}

bool WriteFiles(const std::wstring& dir, const std::vector<GeneratedFile>& files,
                std::wstring* error) {
  for (const GeneratedFile& file : files) {
    const std::wstring path = dir + L"\\" + file.name;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      if (error)
        *error = L"cannot create " + path + L" (error " + std::to_wstring(GetLastError()) + L")";
      return false;
    }
    DWORD written = 0;
    const bool ok = file.bytes.empty() ||
                    (WriteFile(handle, file.bytes.data(), static_cast<DWORD>(file.bytes.size()),
                               &written, nullptr) &&
                     written == file.bytes.size());
    const DWORD lastError = GetLastError();
    CloseHandle(handle);
    if (!ok) {
      if (error) *error = L"cannot write " + path + L" (error " + std::to_wstring(lastError) + L")";
      return false;
    }
  }
  return true;
}

}  // namespace pdft::test
