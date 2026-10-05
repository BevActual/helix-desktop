#include "apps/system_monitor/report_reader.h"
#include "util/file_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>

namespace helix::monitor {
namespace {
bool dateValid(const std::string& text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
  int values[3]; unsigned index = 0;
  for (const auto token : {std::string_view(text).substr(0, 4), std::string_view(text).substr(5, 2), std::string_view(text).substr(8, 2)}) {
    const auto result = std::from_chars(token.data(), token.data() + token.size(), values[index++]);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
  }
  return values[0] >= 1970 && std::chrono::year_month_day{std::chrono::year{values[0]},
    std::chrono::month{static_cast<unsigned>(values[1])}, std::chrono::day{static_cast<unsigned>(values[2])}}.ok();
}
std::uint64_t seconds(const nlohmann::json& value) {
  if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > 172800)) return 0;
  const auto duration = value.get<std::int64_t>();
  return duration >= 0 && duration <= 172800 ? static_cast<std::uint64_t>(duration) : 0;
}
}
Reports parseReports(std::string_view text) {
  Reports result; result.state = Reports::State::Invalid;
  if (text.size() > 4 * 1024 * 1024) return result;
  const auto root = nlohmann::json::parse(text, nullptr, false);
  if (!root.is_object() || root.size() > 366) return result;
  result.state = Reports::State::Available;
  for (const auto& [date, value] : root.items()) {
    if (!dateValid(date) || !value.is_object()) continue;
    ReportDay day; day.date = date;
    if (value.contains("apps") && value.at("apps").is_object()) {
      if (value.at("apps").size() > 4096) { result.state = Reports::State::Invalid; result.days.clear(); return result; }
      for (const auto& [key, duration] : value.at("apps").items()) {
        // Historical instances include a separator followed by the window title.
        // Aggregate by application; never display or retain the title suffix.
        const auto name = key.substr(0, key.find('\x1f'));
        if (name.empty() || name.size() > 512 || std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; })) continue;
        day.apps[name] += seconds(duration);
      }
    }
    if (value.contains("hourly") && value.at("hourly").is_array()) {
      for (std::size_t i = 0; i < std::min<std::size_t>(24, value.at("hourly").size()); ++i) day.hourly[i] = seconds(value.at("hourly").at(i));
    }
    result.days.push_back(std::move(day));
  }
  std::ranges::sort(result.days, [](const ReportDay& a, const ReportDay& b) { return a.date > b.date; });
  return result;
}
Reports readReports(const std::filesystem::path& path) {
  if (path.empty()) return {};
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return error ? Reports{Reports::State::Invalid, {}} : Reports{};
  std::ifstream file(path); if (!file) return {Reports::State::Invalid, {}};
  std::string text(4 * 1024 * 1024 + 1, '\0'); file.read(text.data(), text.size()); text.resize(file.gcount());
  return parseReports(text);
}
std::filesystem::path reportPath() {
  const auto state = FileUtils::stateDir();
  return state.empty() ? std::filesystem::path{} : std::filesystem::path(state) / "screen_time.json";
}
} // namespace helix::monitor
