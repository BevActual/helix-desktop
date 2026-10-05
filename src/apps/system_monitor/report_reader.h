#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace helix::monitor {
struct ReportDay {
  std::string date;
  std::map<std::string, std::uint64_t> apps;
  std::array<std::uint64_t, 24> hourly{};
};
struct Reports {
  enum class State { Missing, Available, Invalid };
  State state = State::Missing;
  std::vector<ReportDay> days;
};
Reports parseReports(std::string_view text);
Reports readReports(const std::filesystem::path& path);
std::filesystem::path reportPath();
} // namespace helix::monitor
