#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::monitor {
struct Cpu {
  std::uint64_t total = 0;
  std::uint64_t idle = 0;
};
struct Memory {
  std::uint64_t total = 0, available = 0, swapTotal = 0, swapFree = 0;
};
struct Process {
  int pid = 0;
  std::string name;
  char state = '?';
  std::uint64_t ticks = 0, started = 0, resident = 0, threads = 0;
};
struct Snapshot {
  std::chrono::steady_clock::time_point sampled;
  std::optional<Cpu> cpu;
  std::optional<Memory> memory;
  std::optional<double> uptime;
  std::optional<std::uint64_t> received, transmitted;
  std::vector<Process> processes;
  bool processesAvailable = false;
};
std::optional<Cpu> parseCpu(std::string_view text);
std::optional<Memory> parseMemory(std::string_view text);
std::optional<Process> parseProcess(std::string_view text, std::uint64_t pageSize);
std::optional<double> cpuUsage(const Cpu& before, const Cpu& after);
std::optional<double> processUsage(const Process& before, const Process& after, double seconds, long ticksPerSecond);
Snapshot readSnapshot(const std::filesystem::path& proc = "/proc");
} // namespace helix::monitor
