#include "apps/system_monitor/proc_reader.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <unistd.h>

namespace helix::monitor {
namespace {
std::string read(const std::filesystem::path& path) {
  std::ifstream file(path);
  // Proc records are small; never read a potentially unbounded stream.
  std::string result(65536, '\0');
  file.read(result.data(), result.size());
  result.resize(static_cast<std::size_t>(file.gcount()));
  return result;
}
bool number(std::string_view token, std::uint64_t& value) {
  const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
  return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}
bool add(std::uint64_t& total, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value; return true;
}
}
std::optional<Cpu> parseCpu(std::string_view text) {
  std::istringstream stream{std::string(text)};
  std::string line; std::getline(stream, line);
  std::istringstream fields(line); std::string token;
  fields >> token; if (token != "cpu") return {};
  Cpu cpu; unsigned count = 0;
  while (fields >> token) {
    std::uint64_t value;
    if (!number(token, value)) return {};
    // guest/guest_nice are already counted in user/nice.
    if (count < 8 && !add(cpu.total, value)) return {};
    if ((count == 3 || count == 4) && !add(cpu.idle, value)) return {};
    ++count;
  }
  if (count < 4 || cpu.total == 0) return {};
  return cpu;
}
std::optional<Memory> parseMemory(std::string_view text) {
  std::istringstream stream{std::string(text)}; std::string line;
  std::map<std::string, std::uint64_t> values;
  while (std::getline(stream, line)) {
    std::istringstream fields(line); std::string key, token, unit;
    fields >> key >> token >> unit;
    if (unit != "kB") continue;
    std::uint64_t value;
    if (!number(token, value) || value > std::numeric_limits<std::uint64_t>::max() / 1024) return {};
    values[key] = value * 1024;
  }
  if (!values.contains("MemTotal:") || !values.contains("MemAvailable:") || !values["MemTotal:"]) return {};
  Memory memory{values["MemTotal:"], values["MemAvailable:"], values["SwapTotal:"], values["SwapFree:"]};
  memory.available = std::min(memory.available, memory.total);
  memory.swapFree = std::min(memory.swapFree, memory.swapTotal);
  return memory;
}
std::optional<Process> parseProcess(std::string_view text, std::uint64_t pageSize) {
  const auto start = text.find('('), end = text.rfind(')');
  if (start == text.npos || end == text.npos || end <= start || !pageSize) return {};
  Process process; std::uint64_t pid;
  auto pidText = text.substr(0, start);
  while (!pidText.empty() && pidText.back() == ' ') pidText.remove_suffix(1);
  if (!number(pidText, pid) || pid == 0 || pid > std::numeric_limits<int>::max()) return {};
  process.pid = static_cast<int>(pid); process.name = text.substr(start + 1, end - start - 1);
  std::istringstream fields{std::string(text.substr(end + 1))};
  std::vector<std::string> tokens; std::string token;
  while (fields >> token) tokens.push_back(token);
  // Remaining fields begin at state (field 3), through RSS (field 24).
  if (tokens.size() < 22 || tokens[0].size() != 1) return {};
  process.state = tokens[0][0]; std::uint64_t user, system, resident;
  if (!number(tokens[11], user) || !number(tokens[12], system) || !add(user, system) ||
      !number(tokens[17], process.threads) || !number(tokens[19], process.started) ||
      !number(tokens[21], resident) || resident > std::numeric_limits<std::uint64_t>::max() / pageSize) return {};
  process.ticks = user; process.resident = resident * pageSize;
  return process;
}
std::optional<double> cpuUsage(const Cpu& before, const Cpu& after) {
  if (after.total <= before.total || after.idle < before.idle) return {};
  const auto total = after.total - before.total, idle = after.idle - before.idle;
  if (idle > total) return {};
  return 100.0 * static_cast<double>(total - idle) / static_cast<double>(total);
}
std::optional<double> processUsage(const Process& before, const Process& after, double seconds, long ticksPerSecond) {
  if (before.pid != after.pid || before.started != after.started || after.ticks < before.ticks ||
      !std::isfinite(seconds) || seconds <= 0 || ticksPerSecond <= 0) return {};
  return 100.0 * static_cast<double>(after.ticks - before.ticks) / (seconds * ticksPerSecond);
}
Snapshot readSnapshot(const std::filesystem::path& proc) {
  Snapshot result;
  result.cpu = parseCpu(read(proc / "stat")); result.memory = parseMemory(read(proc / "meminfo"));
  std::istringstream uptime(read(proc / "uptime")); double seconds;
  if (uptime >> seconds && std::isfinite(seconds) && seconds >= 0) result.uptime = seconds;
  std::istringstream network(read(proc / "net/dev")); std::string line;
  std::uint64_t received = 0, transmitted = 0; bool valid = false, overflow = false;
  while (std::getline(network, line)) {
    const auto colon = line.find(':'); if (colon == line.npos) continue;
    auto name = line.substr(0, colon); name.erase(0, name.find_first_not_of(' '));
    if (name == "lo") continue;
    std::istringstream fields(line.substr(colon + 1)); std::vector<std::string> tokens; std::string token;
    while (fields >> token) tokens.push_back(token);
    std::uint64_t rx, tx;
    if (tokens.size() >= 16 && number(tokens[0], rx) && number(tokens[8], tx)) {
      valid = true; overflow |= !add(received, rx) || !add(transmitted, tx);
    }
  }
  if (valid && !overflow) { result.received = received; result.transmitted = transmitted; }
  std::error_code error;
  std::filesystem::directory_iterator entries(proc, error);
  result.processesAvailable = !error;
  const long pageSize = sysconf(_SC_PAGESIZE);
  for (auto end = std::filesystem::directory_iterator(); entries != end; entries.increment(error)) {
    if (error) { result.processesAvailable = false; break; }
    const auto name = entries->path().filename().string(); std::uint64_t pid;
    if (!number(name, pid)) continue;
    if (auto process = parseProcess(read(entries->path() / "stat"), pageSize > 0 ? pageSize : 4096))
      result.processes.push_back(std::move(*process));
  }
  if (error) result.processesAvailable = false;
  result.sampled = std::chrono::steady_clock::now();
  return result;
}
} // namespace helix::monitor
