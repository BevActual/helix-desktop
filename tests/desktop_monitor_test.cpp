#include "apps/system_monitor/proc_reader.h"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unistd.h>

using namespace helix::monitor;
namespace {
std::string processText(const std::string& name = "worker (test)") {
  std::string text = "42 (" + name + ")";
  for (unsigned i = 0; i < 22; ++i) {
    text += " ";
    if (i == 0) text += "S";
    else if (i == 11) text += "100";
    else if (i == 12) text += "50";
    else if (i == 17) text += "4";
    else if (i == 19) text += "200";
    else if (i == 21) text += "10";
    else text += "0";
  }
  return text;
}
void write(const std::filesystem::path& path, const std::string& text) { std::ofstream(path) << text; }
}
int main() {
  auto cpu = parseCpu("cpu 10 20 30 40 10 2 3 4 100 200\ncpu0 1 2 3 4\n");
  assert(cpu && cpu->total == 119 && cpu->idle == 50); // guest counters must not be counted twice.
  assert(!parseCpu("cpu0 1 2 3 4")); assert(!parseCpu("cpu 1 2 bad 4"));
  assert(!parseCpu("cpu 18446744073709551615 1 2 3"));
  assert(!cpuUsage(*cpu, *cpu)); assert(!cpuUsage(*cpu, {118, 50}));
  assert(!cpuUsage(*cpu, {120, 52})); assert(cpuUsage(*cpu, {219, 100}) == 50);
  auto memory = parseMemory("MemTotal: 1000 kB\nMemAvailable: 250 kB\nSwapTotal: 500 kB\nSwapFree: 100 kB\n");
  assert(memory && memory->total == 1024000 && memory->available == 256000 && memory->swapFree == 102400);
  assert(!parseMemory("MemTotal: 0 kB\nMemAvailable: 0 kB\n"));
  assert(!parseMemory("MemTotal: 1000 kB\n")); assert(!parseMemory("MemTotal: invalid kB\n"));
  memory = parseMemory("MemTotal: 100 kB\nMemAvailable: 1000 kB\n"); assert(memory && memory->available == memory->total);
  auto process = parseProcess(processText(), 4096);
  assert(process && process->pid == 42 && process->name == "worker (test)" && process->ticks == 150 &&
    process->started == 200 && process->resident == 40960 && process->threads == 4);
  assert(parseProcess(processText("name ) with spaces ("), 4096));
  assert(!parseProcess("42 (short) S 1 2", 4096)); assert(!parseProcess(processText(), 0));
  auto after = *process; after.ticks += 200;
  assert(processUsage(*process, after, 2, 100) == 100);
  after.started++; assert(!processUsage(*process, after, 2, 100)); // PID reuse is not CPU activity.
  after = *process; after.ticks--; assert(!processUsage(*process, after, 2, 100));
  assert(!processUsage(*process, *process, 0, 100));
  assert(!processUsage(*process, *process, std::numeric_limits<double>::quiet_NaN(), 100));
  char pattern[] = "/tmp/helix-monitor-XXXXXX"; auto* directory = mkdtemp(pattern); assert(directory);
  const std::filesystem::path root(directory);
  std::filesystem::create_directories(root / "net"); std::filesystem::create_directory(root / "42");
  std::filesystem::create_directory(root / "99"); // Exited/unreadable process is omitted.
  write(root / "stat", "cpu 10 20 30 40\n"); write(root / "meminfo", "MemTotal: 1000 kB\nMemAvailable: 250 kB\n");
  write(root / "uptime", "123.5 200.0\n"); write(root / "42/stat", processText());
  write(root / "net/dev", "lo: 999 0 0 0 0 0 0 0 999 0 0 0 0 0 0 0\neth0: 100 0 0 0 0 0 0 0 200 0 0 0 0 0 0 0\n");
  auto snapshot = readSnapshot(root);
  assert(snapshot.cpu && snapshot.memory && snapshot.uptime == 123.5 && snapshot.processesAvailable);
  assert(snapshot.processes.size() == 1 && snapshot.received == 100 && snapshot.transmitted == 200);
  snapshot = readSnapshot(root / "absent");
  assert(!snapshot.cpu && !snapshot.memory && !snapshot.uptime && !snapshot.processesAvailable && snapshot.processes.empty());
  std::filesystem::remove_all(root);
}
