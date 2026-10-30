// Synthetic PDFs for the tests and the settings app's preview samples. Everything is generated
// in memory, deterministically, from project-authored content: no fixtures, no user files.
//
//   MakeTestPdfs()    the 18 edge cases of the 1.x tools\make-test-pdfs.ps1, byte for byte
//                     (orientation, /Rotate, CropBox, first page, annotations, extreme sizes,
//                     a heavy page, XFA, and files that must fail cleanly)
//   MakeSamplePdfs()  sample-document.pdf (A4 portrait) and sample-slide.pdf (16:9), the two
//                     pages the settings app previews at the chosen zoom
//
// thumbtest writes them with "makepdfs <dir>" and "makesamples <dir>".

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pdft::test {

// One generated file: its name and its bytes (Latin-1 text for the PDFs; empty for 12-empty).
struct GeneratedFile {
  std::wstring name;
  std::string bytes;
};

// The 18 test PDFs, 01-portrait.pdf ... 18-xfa-static.pdf, in name order.
std::vector<GeneratedFile> MakeTestPdfs();

// sample-document.pdf and sample-slide.pdf.
std::vector<GeneratedFile> MakeSamplePdfs();

// Serialises object bodies into a PDF 1.7 file with an exact cross-reference table:
// object n is objects[n - 1], and object 1 must be the catalog.
std::string SerializePdf(const std::vector<std::string>& objects);

// A content stream object: "<< <dict> /Length <n> >>" + the data.
std::string StreamObject(std::string_view data, std::string_view dict = {});

// Writes |files| into |dir|, which must exist. False (with a message in |error|) on the
// first file that cannot be written.
bool WriteFiles(const std::wstring& dir, const std::vector<GeneratedFile>& files,
                std::wstring* error);

}  // namespace pdft::test
