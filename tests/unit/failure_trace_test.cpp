// Where an administrator step's failure started (failure_trace.h): the place a note names (the
// file's name only), what a trace keeps as a failure is passed on or handled, the parts of a
// step, and the report's text; also the token facts the report starts with (elevation.h).

#include <windows.h>

#include "failure_trace.h"

#include <gtest/gtest.h>

#include <cstring>
#include <cwchar>
#include <string>

#include "elevation.h"

namespace pdft {
namespace {

// The lines of the notes below, for the expectations.
unsigned g_innerLine = 0;
unsigned g_outerLine = 0;

HRESULT Inner(HRESULT hr) {
  g_innerLine = __LINE__ + 1;
  return PDFT_TRACE_DETAIL(hr, L"PdfThumbnailsCli.exe");
}

// Passes Inner's failure on, as RETURN_IF_FAILED(PDFT_TRACE(...)) does, with another HRESULT
// when |mapped| is a failure.
HRESULT Outer(HRESULT inner, HRESULT mapped) {
  const HRESULT hr = Inner(inner);
  g_outerLine = __LINE__ + 1;
  return PDFT_TRACE(FAILED(hr) && FAILED(mapped) ? mapped : hr);
}

TEST(FailureTraceTest, ASiteNamesTheFileOnly) {
  const SourceSite site = PDFT_SOURCE_SITE;
  const unsigned line = __LINE__ - 1;
  EXPECT_STREQ(site.file, "failure_trace_test.cpp");
  EXPECT_EQ(std::strpbrk(site.file, "\\/:"), nullptr);
  EXPECT_EQ(site.line, line);
  EXPECT_STREQ(site.function, "TestBody");
}

TEST(FailureTraceTest, WithoutATraceANoteOnlyReturnsItsValue) {
  EXPECT_EQ(PDFT_TRACE(E_ACCESSDENIED), E_ACCESSDENIED);
  EXPECT_EQ(PDFT_TRACE(S_FALSE), S_FALSE);
  const FailureTrace trace;
  EXPECT_FALSE(trace.failure());  // nothing from before it
}

TEST(FailureTraceTest, KeepsWhereTheFailureStarted) {
  const FailureTrace trace;
  const TraceStep step(L"copy-build");
  EXPECT_EQ(Outer(E_ACCESSDENIED, S_OK), E_ACCESSDENIED);
  ASSERT_TRUE(trace.failure());
  EXPECT_EQ(trace.failure()->hr, E_ACCESSDENIED);
  EXPECT_STREQ(trace.failure()->site.file, "failure_trace_test.cpp");
  EXPECT_EQ(trace.failure()->site.line, g_innerLine);
  EXPECT_STREQ(trace.failure()->site.function, "Inner");
  EXPECT_STREQ(trace.failure()->step, L"copy-build");
  EXPECT_STREQ(trace.failure()->detail.data(), L"PdfThumbnailsCli.exe");

  // A caller that passes it on as another HRESULT changes nothing either.
  const FailureTrace mapped;
  EXPECT_EQ(Outer(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), E_FAIL), E_FAIL);
  ASSERT_TRUE(mapped.failure());
  EXPECT_EQ(mapped.failure()->hr, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));
  EXPECT_EQ(mapped.failure()->site.line, g_innerLine);
}

TEST(FailureTraceTest, AFailureNotedFirstByTheCallerIsTheCallers) {
  const FailureTrace trace;
  // Inner succeeded; the caller's own failure starts there.
  EXPECT_EQ(Outer(S_OK, S_OK), S_OK);
  EXPECT_FALSE(trace.failure());
  EXPECT_EQ(PDFT_TRACE(HRESULT_FROM_WIN32(ERROR_DIRECTORY)), HRESULT_FROM_WIN32(ERROR_DIRECTORY));
  ASSERT_TRUE(trace.failure());
  EXPECT_STREQ(trace.failure()->site.function, "TestBody");
  EXPECT_STREQ(trace.failure()->step, L"");
  EXPECT_STREQ(trace.failure()->detail.data(), L"");
}

TEST(FailureTraceTest, AHandledFailureIsDropped) {
  const FailureTrace trace;
  Inner(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));  // a retry follows
  ASSERT_TRUE(trace.failure());
  EXPECT_EQ(PDFT_TRACE(S_OK), S_OK);  // ... and works
  EXPECT_FALSE(trace.failure());
  // The next failure is a new one, kept where it starts.
  EXPECT_EQ(Outer(E_ACCESSDENIED, S_OK), E_ACCESSDENIED);
  ASSERT_TRUE(trace.failure());
  EXPECT_EQ(trace.failure()->site.line, g_innerLine);
  EXPECT_EQ(trace.failure()->hr, E_ACCESSDENIED);
}

// A failure tried again (or tolerated) without a success noted in between: once it is noted as
// handled, the next failure is kept where it starts, not taken for that one passed on.
TEST(FailureTraceTest, AFailureNotedAsHandledIsDropped) {
  const FailureTrace trace;
  Inner(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));  // in use: tried again
  ASSERT_TRUE(trace.failure());
  NoteHandled();
  EXPECT_FALSE(trace.failure());
  EXPECT_EQ(PDFT_TRACE(E_ACCESSDENIED), E_ACCESSDENIED);
  ASSERT_TRUE(trace.failure());
  EXPECT_STREQ(trace.failure()->site.function, "TestBody");
  EXPECT_EQ(trace.failure()->hr, E_ACCESSDENIED);
  NoteHandled();
  EXPECT_FALSE(trace.failure());
}

// A part of the step that starts means the step went on: what was pending was handled.
TEST(FailureTraceTest, APartThatStartsDropsThePendingFailure) {
  const FailureTrace trace;
  TraceStep step(L"after-class");
  Inner(E_FAIL);  // tolerated and listed
  ASSERT_TRUE(trace.failure());
  step.Set(L"next");
  EXPECT_FALSE(trace.failure());
  Inner(E_FAIL);
  ASSERT_TRUE(trace.failure());
  {
    const TraceStep inner(L"inner");
    EXPECT_FALSE(trace.failure());
    Inner(E_ACCESSDENIED);
  }
  // The inner part's failure is passed on as it was: going away drops nothing.
  ASSERT_TRUE(trace.failure());
  EXPECT_EQ(trace.failure()->hr, E_ACCESSDENIED);
  EXPECT_STREQ(trace.failure()->step, L"inner");
}

TEST(FailureTraceTest, StepsNameTheNotesAfterThem) {
  const FailureTrace trace;
  TraceStep outer(L"read-build");
  {
    const TraceStep inner(L"inner");
    EXPECT_EQ(PDFT_TRACE(S_OK), S_OK);
  }
  // The inner one has gone: the outer name is back.
  Inner(E_FAIL);
  ASSERT_TRUE(trace.failure());
  EXPECT_STREQ(trace.failure()->step, L"read-build");
  EXPECT_EQ(PDFT_TRACE(S_OK), S_OK);
  outer.Set(L"copy-build");
  Inner(E_FAIL);
  ASSERT_TRUE(trace.failure());
  EXPECT_STREQ(trace.failure()->step, L"copy-build");
}

TEST(FailureTraceTest, ANewerTraceHidesTheOlderUntilItGoes) {
  const FailureTrace older;
  {
    const FailureTrace newer;
    Inner(E_ACCESSDENIED);
    EXPECT_TRUE(newer.failure());
    EXPECT_FALSE(older.failure());
  }
  Inner(E_OUTOFMEMORY);
  ASSERT_TRUE(older.failure());
  EXPECT_EQ(older.failure()->hr, E_OUTOFMEMORY);
}

TEST(FailureTraceTest, ALongDetailIsCut) {
  const FailureTrace trace;
  const std::wstring detail(kTraceDetailChars + 20, L'x');
  EXPECT_EQ(PDFT_TRACE_DETAIL(E_FAIL, detail), E_FAIL);
  ASSERT_TRUE(trace.failure());
  EXPECT_EQ(std::wstring(trace.failure()->detail.data()), std::wstring(kTraceDetailChars, L'x'));
}

TEST(FailureTraceTest, Text) {
  TracedFailure failure;
  failure.site = SourceSite{"install.cpp", 47, "MakeRealFolder"};
  failure.hr = E_ACCESSDENIED;
  failure.step = L"copy-build";
  wcscpy_s(failure.detail.data(), failure.detail.size(), L"PdfThumbnails");
  EXPECT_EQ(TracedFailureText(failure),
            L"in copy-build at install.cpp:47 MakeRealFolder hr=0x80070005 (PdfThumbnails)");
  failure.step = L"";
  failure.detail[0] = L'\0';
  failure.hr = HRESULT_FROM_WIN32(ERROR_DIRECTORY);
  EXPECT_EQ(TracedFailureText(failure), L"at install.cpp:47 MakeRealFolder hr=0x8007010B");
}

TEST(TokenFactsTest, Text) {
  TokenFacts facts;
  facts.elevated = true;
  facts.elevationType = TokenElevationTypeFull;
  facts.integrityRid = SECURITY_MANDATORY_HIGH_RID;
  facts.administrators = TokenFacts::Group::kEnabled;
  EXPECT_EQ(TokenFactsText(facts),
            L"token: elevated yes, elevation type 2 (full), integrity 0x3000, Administrators "
            L"enabled");
  facts.elevated = false;
  facts.elevationType = TokenElevationTypeLimited;
  facts.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
  facts.administrators = TokenFacts::Group::kDenyOnly;
  EXPECT_EQ(TokenFactsText(facts),
            L"token: elevated no, elevation type 3 (limited), integrity 0x2000, Administrators "
            L"deny-only");
  facts.elevationType = TokenElevationTypeDefault;
  facts.administrators = TokenFacts::Group::kAbsent;
  EXPECT_EQ(TokenFactsText(facts),
            L"token: elevated no, elevation type 1 (default), integrity 0x2000, Administrators "
            L"absent");
  facts.administrators = TokenFacts::Group::kDisabled;
  EXPECT_NE(TokenFactsText(facts).find(L"Administrators not enabled"), std::wstring::npos);
}

}  // namespace
}  // namespace pdft
