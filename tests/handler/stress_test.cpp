// Many threads rendering at once, as in Explorer's thumbnail process: PDFium is serialised by
// the handler's lock, and every successful render of a file has the same size.
//
// The 1.1 lock is unfair and its 15 s wait counts from the start of the call (a 2.1 item), so
// on a small machine a slow page can time out under contention. The default run therefore
// leaves out the two slow files (08, 09) and tolerates WTS_E_EXTRACTIONTIMEDOUT; set
// PDFT_STRESS_FULL=1 for the full 1.1 harness run (all files, 8 threads x 5 rounds).

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "handler_env.h"

namespace pdft::test {
namespace {

struct Expectation {
  const wchar_t* prefix;
  HRESULT hr;
  int width, height;
};

// At zoom 100, edge 256.
const Expectation kAll[] = {
    {L"01-", S_OK, 181, 256},
    {L"02-", S_OK, 256, 181},
    {L"03-", S_OK, 256, 181},
    {L"04-", S_OK, 256, 181},
    {L"05-", S_OK, 181, 256},
    {L"06-", S_OK, 3, 256},
    {L"07-", S_OK, 181, 256},
    {L"08-", S_OK, 181, 256},
    {L"09-", S_OK, 256, 256},
    {L"10-", S_OK, 198, 256},
    {L"11-", WTS_E_FAILEDEXTRACTION, 0, 0},
    {L"12-", WTS_E_FAILEDEXTRACTION, 0, 0},
    {L"13-", WTS_E_FAILEDEXTRACTION, 0, 0},
    {L"14-", WTS_E_FAILEDEXTRACTION, 0, 0},
    {L"15-", WTS_E_FAILEDEXTRACTION, 0, 0},
    {L"16-", S_OK, 181, 256},
    {L"17-", WTS_E_FAILEDEXTRACTION, 0, 0},
    {L"18-", S_OK, 181, 256},
};

struct Tally {
  int ok = 0, timeouts = 0;
  std::set<std::pair<int, int>> sizes;
  std::set<HRESULT> failures;
};

void RunStress(bool includeSlowFiles, int threads, int rounds) {
  std::vector<const Expectation*> cases;
  std::vector<std::wstring> paths;
  for (const auto& e : kAll) {
    const std::wstring prefix = e.prefix;
    if (!includeSlowFiles && (prefix == L"08-" || prefix == L"09-")) continue;
    cases.push_back(&e);
    paths.push_back(HandlerEnvironment::Pdf(e.prefix));
  }
  ScopedZoom zoom(100);
  const HandlerHost& host = HandlerEnvironment::Host();
  std::map<size_t, Tally> tallies;
  std::mutex lock;
  std::atomic<size_t> next{0};
  const size_t total = static_cast<size_t>(rounds) * cases.size();
  const ULONGLONG start = GetTickCount64();
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      for (size_t i = next++; i < total; i = next++) {
        const size_t index = i % cases.size();
        const Rendered r = host.RenderFile(paths[index], 256);
        std::lock_guard<std::mutex> guard(lock);
        Tally& tally = tallies[index];
        if (SUCCEEDED(r.hr)) {
          ++tally.ok;
          tally.sizes.insert({r.width, r.height});
        } else if (r.hr == WTS_E_EXTRACTIONTIMEDOUT) {
          ++tally.timeouts;
        } else {
          tally.failures.insert(r.hr);
        }
      }
      CoUninitialize();
    });
  }
  for (auto& worker : workers) worker.join();
  std::printf("[ info     ] %zu renders on %d threads in %llu ms\n", total, threads,
              GetTickCount64() - start);

  for (size_t index = 0; index < cases.size(); ++index) {
    const Expectation& e = *cases[index];
    const Tally& tally = tallies[index];
    const std::string name = Printable(e.prefix);
    if (tally.timeouts) {
      std::printf("[ info     ] %s: %d of %d renders timed out under contention\n", name.c_str(),
                  tally.timeouts, rounds);
    }
    EXPECT_LE(tally.sizes.size(), 1u) << name << ": inconsistent sizes";
    if (SUCCEEDED(e.hr)) {
      EXPECT_TRUE(tally.failures.empty())
          << name << ": failed with " << HresultText(*tally.failures.begin());
      for (const auto& size : tally.sizes) {
        EXPECT_EQ(size.first, e.width) << name;
        EXPECT_EQ(size.second, e.height) << name;
      }
    } else {
      EXPECT_EQ(tally.ok, 0) << name << ": must never render";
      for (HRESULT hr : tally.failures) EXPECT_EQ(HresultText(hr), HresultText(e.hr)) << name;
    }
  }
}

TEST(Stress, EightThreadsFiveRounds) { RunStress(false, 8, 5); }

TEST(Stress, EightThreadsFiveRoundsAllFiles) {
  wchar_t value[4] = {};
  if (GetEnvironmentVariableW(L"PDFT_STRESS_FULL", value, 4) != 1 || value[0] != L'1') {
    GTEST_SKIP() << "set PDFT_STRESS_FULL=1 to include 08-heavy and 09-max-page";
  }
  RunStress(true, 8, 5);
}

}  // namespace
}  // namespace pdft::test
