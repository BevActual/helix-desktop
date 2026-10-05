#include "apps/system_monitor/report_reader.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

using namespace helix::monitor;
int main() {
  assert(parseReports("not JSON").state == Reports::State::Invalid);
  assert(parseReports("[]").state == Reports::State::Invalid);
  assert(parseReports("{}").state == Reports::State::Available);
  auto reports = parseReports(R"({
    "2026-10-03":{"apps":{"browser":120,"browser\u001fprivate window title":30,"invalid":-20},"hourly":[100,50]},
    "2026-10-04":{"apps":{"editor":200,"overflow":18446744073709551615},"hourly":[200]},
    "2026-02-30":{"apps":{"invalid-date":20}},
    "wrong-date":{},"2026-10-05":false
  })");
  assert(reports.days.size() == 2 && reports.days[0].date == "2026-10-04");
  assert(reports.days[1].apps.at("browser") == 150 && reports.days[1].hourly[0] == 100);
  assert(!reports.days[1].apps.contains("browser\x1fprivate window title"));
  assert(reports.days[0].apps.at("overflow") == 0 && reports.days[1].apps.at("invalid") == 0);
  assert(parseReports(std::string(4 * 1024 * 1024 + 1, 'x')).state == Reports::State::Invalid);
  assert(parseReports(R"({"2024-02-29":{},"2025-02-29":{}})").days.size() == 1);
  char pattern[] = "/tmp/helix-monitor-reports-XXXXXX"; const auto* directory = mkdtemp(pattern); assert(directory);
  const std::filesystem::path root(directory);
  assert(readReports(root / "absent").state == Reports::State::Missing);
  std::ofstream(root / "saved.json") << R"({"2026-10-04":{"apps":{"editor":200}}})";
  assert(readReports(root / "saved.json").days.front().apps.at("editor") == 200);
  setenv("NOCTALIA_STATE_HOME", directory, 1);
  assert(reportPath() == root / "noctalia/screen_time.json");
  unsetenv("NOCTALIA_STATE_HOME"); setenv("XDG_STATE_HOME", directory, 1);
  assert(reportPath() == root / "noctalia/screen_time.json");
  std::filesystem::remove_all(root);
}
