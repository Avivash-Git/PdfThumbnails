// The test-PDF generator: byte equality with the 1.x PowerShell generator, exact xref tables,
// determinism, and the preview samples.

#include <windows.h>

#include "test_pdfs.h"

#include <bcrypt.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "temp_dir.h"

namespace pdft::test {
namespace {

std::string Sha256Hex(const std::string& data) {
  UCHAR digest[32] = {};
  const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                     reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                     static_cast<ULONG>(data.size()), digest, sizeof(digest));
  if (status < 0) return "BCryptHash failed";
  std::string hex;
  char byte[3];
  for (UCHAR value : digest) {
    std::snprintf(byte, sizeof(byte), "%02x", value);
    hex += byte;
  }
  return hex;
}

std::string Narrow(const std::wstring& text) {
  std::string out;
  for (wchar_t c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
  return out;
}

// Parses "<digits>" at |pos|; npos when there are none.
size_t ReadNumber(const std::string& text, size_t pos, size_t* end) {
  size_t value = 0, i = pos;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9')
    value = value * 10 + (text[i++] - '0');
  *end = i;
  return i == pos ? std::string::npos : value;
}

// The structure SerializePdf() promises: header, objects at the offsets the xref table names,
// exact /Length values, trailer and startxref. Returns "" when the file is well formed.
std::string CheckPdfStructure(const std::string& pdf) {
  if (pdf.rfind("%PDF-1.7\n", 0) != 0) return "no %PDF-1.7 header";
  const std::string eof = "\n%%EOF\n";
  if (pdf.size() < eof.size() || pdf.compare(pdf.size() - eof.size(), eof.size(), eof) != 0) {
    return "does not end with %%EOF";
  }
  const size_t startxref = pdf.rfind("startxref\n");
  if (startxref == std::string::npos) return "no startxref";
  size_t end = 0;
  const size_t xref = ReadNumber(pdf, startxref + 10, &end);
  if (xref == std::string::npos || pdf.compare(xref, 7, "xref\n0 ") != 0) {
    return "startxref does not point at the xref table";
  }
  const size_t count = ReadNumber(pdf, xref + 7, &end);
  if (count == std::string::npos || count < 2 || pdf.compare(end, 1, "\n") != 0) {
    return "bad xref subsection header";
  }
  size_t entry = end + 1;
  if (pdf.compare(entry, 20, "0000000000 65535 f \n") != 0) return "bad free-list head";
  for (size_t object = 1; object < count; ++object) {
    entry += 20;
    if (pdf.compare(entry + 10, 10, " 00000 n \n") != 0) return "bad xref entry";
    size_t offsetEnd = 0;
    const size_t offset = ReadNumber(pdf, entry, &offsetEnd);
    if (offsetEnd != entry + 10) return "xref offset is not 10 digits";
    const std::string header = std::to_string(object) + " 0 obj\n";
    if (offset >= pdf.size() || pdf.compare(offset, header.size(), header) != 0) {
      return "xref entry " + std::to_string(object) + " does not point at its object";
    }
    // A stream's /Length is the exact number of bytes between "stream\n" and "\nendstream".
    const size_t objectEnd = pdf.find("\nendobj\n", offset);
    const size_t stream = pdf.find(">>\nstream\n", offset);
    if (stream != std::string::npos && stream < objectEnd) {
      const size_t length = pdf.rfind("/Length ", stream);
      size_t lengthEnd = 0;
      const size_t declared = ReadNumber(pdf, length + 8, &lengthEnd);
      const size_t data = stream + 10;
      const size_t dataEnd = pdf.find("\nendstream\nendobj\n", data);
      if (length < offset || dataEnd == std::string::npos || declared != dataEnd - data) {
        return "object " + std::to_string(object) + " has a wrong /Length";
      }
    }
  }
  const std::string trailer = "trailer\n<< /Size " + std::to_string(count) + " /Root 1 0 R >>\n";
  if (pdf.compare(entry + 20, trailer.size(), trailer) != 0 ||
      entry + 20 + trailer.size() != startxref) {
    return "bad trailer";
  }
  return "";
}

const GeneratedFile* Find(const std::vector<GeneratedFile>& files, const std::wstring& prefix) {
  for (const auto& file : files) {
    if (file.name.rfind(prefix, 0) == 0) return &file;
  }
  return nullptr;
}

TEST(TestPdfs, NamesAndOrder) {
  const wchar_t* expected[] = {
      L"01-portrait.pdf",       L"02-landscape.pdf",
      L"03-rotate90.pdf",       L"04-cropbox.pdf",
      L"05-multipage.pdf",      L"06-tall-strip.pdf",
      L"07-annotation.pdf",     L"08-heavy.pdf",
      L"09-max-page-14400.pdf", L"10-zero-size-page.pdf",
      L"11-no-pages.pdf",       L"12-empty.pdf",
      L"13-garbage.pdf",        L"14-text-file.pdf",
      L"15-truncated.pdf",      L"16-unicode-δοκιμή-テスト.pdf",
      L"17-xfa-dynamic.pdf",    L"18-xfa-static.pdf",
  };
  const auto files = MakeTestPdfs();
  ASSERT_EQ(files.size(), std::size(expected));
  for (size_t i = 0; i < files.size(); ++i) EXPECT_EQ(files[i].name, expected[i]) << i;
}

// SHA-256 of every file tools\make-test-pdfs.ps1 (1.1, Windows PowerShell 5.1) wrote, recorded
// on 30 Oct 2026 before the script was removed. The C++ port must keep producing these bytes.
TEST(TestPdfs, ByteIdenticalToThePowerShellGenerator) {
  const struct {
    const wchar_t* prefix;
    size_t size;
    const char* sha256;
  } expected[] = {
      {L"01-", 707, "7dbe96577f02c12cd257f4bdfa8479d3491139b354ceddb3d257594f8e892634"},
      {L"02-", 708, "32b7602e58bc1921e998f95ebd1e86dd408e0d3b1e98fb346580d7b351ef65b0"},
      {L"03-", 718, "9918fd567880fd6117e29c56420ef89b9c3234ba553f8cf646f1b8f5d5c81071"},
      {L"04-", 730, "3ef16a3d2f7ef41111e13077cd8ffec5089476a637466f8d66d410b92e30596c"},
      {L"05-", 915, "dee2ca000513988923b3d6ff6ce0395208804eea0baf61ce73575e8668094722"},
      {L"06-", 1621, "05352f5b126c9bd3757d01f466ca02cec982bb524ae142f690f151ee518bb98f"},
      {L"07-", 1003, "bba37867b3216ee0bca2a8948800a23cf02690162bedbf7755093da15e14c98e"},
      {L"08-", 5009360, "f56b86b46a196187cc81eb6e6fd40fa14e3e3681cf12e9358185f832ce4aaf9e"},
      {L"09-", 725, "7036acacf24d0e9e0ab76983acbb16b474f0e3374d5ef5bcde0bbbd089b2cb31"},
      {L"10-", 551, "ed0618d549306b4369b8724b8e80f43ea5352410751ee9a18aeacd4c77014140"},
      {L"11-", 233, "50dc246b4ff9509811a23d9fcf7d6c8465ed2b4eed08aa049d9feae8e8afd526"},
      {L"12-", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {L"13-", 16384, "3ca466d2ef5e33a865c8423c9051440f081ea84ed1462206bca07af2f79f8f13"},
      {L"14-", 19, "0d28a0d3b65144f83417ae4ddb8cd9e4e5dd3510c2fc8c4397edbda12793de6f"},
      {L"15-", 390, "e8d6066418558d1cea37e3be8697e7ebe71513a36730a5b1dd27f999ca392ff1"},
      {L"16-", 707, "7dbe96577f02c12cd257f4bdfa8479d3491139b354ceddb3d257594f8e892634"},
      {L"17-", 959, "3d94c5da0e288e7c0498e2b6ffedf2b6ba8e19913c8996ef5e78daa61bbcd8fa"},
      {L"18-", 938, "f0eb2f6d880c9ebf02bfb679323effc5d172c8611c725db22dcfbea54b62e096"},
  };
  const auto files = MakeTestPdfs();
  for (const auto& e : expected) {
    const GeneratedFile* file = Find(files, e.prefix);
    ASSERT_NE(file, nullptr) << Narrow(e.prefix);
    EXPECT_EQ(file->bytes.size(), e.size) << Narrow(file->name);
    EXPECT_EQ(Sha256Hex(file->bytes), e.sha256) << Narrow(file->name);
  }
}

TEST(TestPdfs, XrefTablesAndStreamLengthsAreExact) {
  for (const auto& file : MakeTestPdfs()) {
    // 12-15 are not PDFs on purpose (empty, random bytes, text, cut off).
    if (file.name >= L"12-" && file.name < L"16-") continue;
    EXPECT_EQ(CheckPdfStructure(file.bytes), "") << Narrow(file.name);
  }
}

TEST(TestPdfs, Deterministic) {
  const auto a = MakeTestPdfs(), b = MakeTestPdfs();
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) EXPECT_TRUE(a[i].bytes == b[i].bytes) << Narrow(a[i].name);
}

TEST(TestPdfs, DerivedFiles) {
  const auto files = MakeTestPdfs();
  const std::string& portrait = Find(files, L"01-")->bytes;
  EXPECT_EQ(Find(files, L"16-")->bytes, portrait);
  const std::string& truncated = Find(files, L"15-")->bytes;
  EXPECT_LT(truncated.size(), portrait.size());
  EXPECT_EQ(portrait.compare(0, truncated.size(), truncated), 0);
  EXPECT_TRUE(Find(files, L"12-")->bytes.empty());
  EXPECT_EQ(Find(files, L"14-")->bytes, "This is not a PDF.\n");
  EXPECT_NE(Find(files, L"17-")->bytes.find("/NeedsRendering true"), std::string::npos);
  EXPECT_EQ(Find(files, L"18-")->bytes.find("/NeedsRendering"), std::string::npos);
}

TEST(TestPdfs, SerializePdfExample) {
  EXPECT_EQ(SerializePdf({"<< /Type /Catalog >>"}),
            "%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\nxref\n0 2\n0000000000 65535 f \n"
            "0000000009 00000 n \ntrailer\n<< /Size 2 /Root 1 0 R >>\nstartxref\n45\n%%EOF\n");
  EXPECT_EQ(StreamObject("q Q"), "<<  /Length 3 >>\nstream\nq Q\nendstream");
  EXPECT_EQ(StreamObject("q Q", "/Filter /None"),
            "<< /Filter /None /Length 3 >>\nstream\nq Q\nendstream");
}

TEST(SamplePdfs, NamesSizesAndStructure) {
  const auto samples = MakeSamplePdfs();
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_EQ(samples[0].name, L"sample-document.pdf");
  EXPECT_EQ(samples[1].name, L"sample-slide.pdf");
  EXPECT_NE(samples[0].bytes.find("/MediaBox [0 0 595 842]"), std::string::npos);
  EXPECT_NE(samples[1].bytes.find("/MediaBox [0 0 960 540]"), std::string::npos);
  for (const auto& sample : samples) {
    EXPECT_EQ(CheckPdfStructure(sample.bytes), "") << Narrow(sample.name);
    EXPECT_LT(sample.bytes.size(), 4096u) << Narrow(sample.name);
    for (char c : sample.bytes) ASSERT_TRUE(c == '\n' || (c >= ' ' && c < 0x7F));  // ASCII text
  }
  const auto again = MakeSamplePdfs();
  EXPECT_EQ(again[0].bytes, samples[0].bytes);
  EXPECT_EQ(again[1].bytes, samples[1].bytes);
}

TEST(TestPdfs, WriteFilesWritesEveryFile) {
  TempDir dir(L"write");
  ASSERT_TRUE(dir.ok());
  const auto files = MakeTestPdfs();
  std::wstring error;
  ASSERT_TRUE(WriteFiles(dir.path(), files, &error)) << Narrow(error);
  for (const auto& file : files) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    ASSERT_TRUE(GetFileAttributesExW(dir.File(file.name).c_str(), GetFileExInfoStandard, &data))
        << Narrow(file.name);
    EXPECT_EQ(data.nFileSizeLow, file.bytes.size()) << Narrow(file.name);
    EXPECT_EQ(data.nFileSizeHigh, 0u);
  }
  EXPECT_FALSE(WriteFiles(dir.File(L"missing-subfolder"), files, &error));
  EXPECT_NE(error.find(L"cannot create"), std::wstring::npos);
}

}  // namespace
}  // namespace pdft::test
