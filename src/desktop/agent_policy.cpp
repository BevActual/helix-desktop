#include "desktop/agent_policy.h"

#include "core/toml.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace helix::agent {

namespace {
constexpr std::array<std::pair<Tier, std::string_view>, 5> kTierNames{{
  {Tier::Observe, "observe"}, {Tier::ObservePrivate, "observe-private"}, {Tier::Act, "act"},
  {Tier::Confirm, "confirm"}, {Tier::Blocked, "blocked"},
}};

std::string truncated(const std::string& value, std::size_t limit) {
  if (value.size() <= limit) return value;
  // Do not cut a UTF-8 sequence in half.
  std::size_t end = limit;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
  return value.substr(0, end) + "…";
}

std::string isoTime(std::chrono::system_clock::time_point when) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}
}  // namespace

std::string_view tierName(Tier tier) {
  for (const auto& [value, name] : kTierNames) if (value == tier) return name;
  return "blocked";
}

std::optional<Tier> parseTier(std::string_view name) {
  for (const auto& [value, text] : kTierNames) if (text == name) return value;
  return std::nullopt;
}

std::expected<RiskPolicy, std::string> RiskPolicy::parse(std::string_view text) {
  toml::table table;
  try {
    table = toml::parse(text);
  } catch (const toml::parse_error& error) {
    return std::unexpected(std::string("risk policy: ") + std::string(error.description()));
  }
  RiskPolicy policy;
  auto readSection = [&](const char* section, std::map<std::string, Tier, std::less<>>& into) -> std::string {
    const auto* node = table[section].as_table();
    if (!node) return std::string();
    for (const auto& [key, value] : *node) {
      const auto name = value.value<std::string>();
      const auto tier = name ? parseTier(*name) : std::nullopt;
      if (!tier) return std::string("risk policy: ") + section + "." + std::string(key.str()) + " has an unknown tier";
      into.emplace(std::string(key.str()), *tier);
    }
    return std::string();
  };
  if (auto error = readSection("tiers", policy.tiers_); !error.empty()) return std::unexpected(error);
  if (auto error = readSection("choices", policy.choices_); !error.empty()) return std::unexpected(error);
  if (policy.tiers_.empty()) return std::unexpected("risk policy: no [tiers] entries");
  return policy;
}

std::expected<RiskPolicy, std::string> RiskPolicy::load(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) return std::unexpected("risk policy: cannot read " + file.string());
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str());
}

Tier RiskPolicy::tierFor(std::string_view command, std::span<const std::string> args) const {
  if (!args.empty()) {
    const auto override = choices_.find(std::string(command) + " " + args.front());
    if (override != choices_.end()) return override->second;
  }
  const auto found = tiers_.find(command);
  return found == tiers_.end() ? Tier::Blocked : found->second;
}

std::vector<std::string> RiskPolicy::commands() const {
  std::vector<std::string> names;
  for (const auto& entry : tiers_) names.push_back(entry.first);
  return names;
}

std::expected<void, std::string> GrantStore::load() {
  grants_.clear();
  std::ifstream in(file_);
  if (!in) return {};  // no file yet: nobody is granted anything
  try {
    const auto json = nlohmann::json::parse(in);
    for (const auto& [client, tiers] : json.at("clients").items()) {
      std::set<Tier> parsed;
      for (const auto& name : tiers) {
        const auto tier = parseTier(name.get<std::string>());
        if (tier && *tier != Tier::Blocked) parsed.insert(*tier);
      }
      grants_[client] = std::move(parsed);
    }
  } catch (const std::exception& error) {
    grants_.clear();
    return std::unexpected(std::string("grants: ") + error.what());
  }
  return {};
}

std::expected<void, std::string> GrantStore::save() const {
  nlohmann::json clients = nlohmann::json::object();
  for (const auto& [client, tiers] : grants_) {
    auto names = nlohmann::json::array();
    for (const Tier tier : tiers) names.push_back(std::string(tierName(tier)));
    clients[client] = std::move(names);
  }
  const nlohmann::json json{{"version", 1}, {"clients", std::move(clients)}};

  std::error_code ec;
  std::filesystem::create_directories(file_.parent_path(), ec);
  const auto temporary = file_.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected(std::string("grants: ") + std::strerror(errno));
  const std::string body = json.dump(2) + "\n";
  const bool written = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
  ::close(fd);
  if (!written || std::rename(temporary.c_str(), file_.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return std::unexpected("grants: could not write " + file_.string());
  }
  return {};
}

void GrantStore::set(const std::string& client, std::set<Tier> tiers) {
  tiers.erase(Tier::Blocked);
  grants_[client] = std::move(tiers);
}

void GrantStore::revoke(const std::string& client) { grants_.erase(client); }

bool GrantStore::allows(const std::string& client, Tier tier) const {
  if (tier == Tier::Blocked) return false;
  const auto found = grants_.find(client);
  return found != grants_.end() && found->second.contains(tier);
}

std::set<Tier> GrantStore::tiersFor(const std::string& client) const {
  const auto found = grants_.find(client);
  return found == grants_.end() ? std::set<Tier>{} : found->second;
}

std::vector<std::string> GrantStore::clients() const {
  std::vector<std::string> names;
  for (const auto& entry : grants_) names.push_back(entry.first);
  return names;
}

std::string clientIdentity(const std::string& exePath, const std::vector<std::string>& cmdline) {
  const std::string base = std::filesystem::path(exePath).filename().string();
  static constexpr std::array<std::string_view, 7> kInterpreters{"python", "node", "bun", "deno", "ruby", "perl", "bash"};
  const bool interpreter = std::ranges::any_of(kInterpreters, [&](std::string_view name) { return base.starts_with(name); })
                           || base == "sh";
  if (interpreter) {
    // First argument that is not an option is the script (or module name for `-m`).
    for (std::size_t i = 1; i < cmdline.size(); ++i) {
      if (cmdline[i].empty() || cmdline[i].front() == '-') continue;
      std::error_code ec;
      const auto script = std::filesystem::weakly_canonical(cmdline[i], ec);
      return "exe:" + exePath + "#" + (ec ? cmdline[i] : script.string());
    }
  }
  return "exe:" + exePath;
}

std::expected<std::string, std::string> buildWireCommand(std::string_view tool, std::span<const std::string> argv,
                                                         std::span<const std::string> literal) {
  auto clean = [](std::string_view text) { return text.find('\x1e') == std::string_view::npos && text.find('\0') == std::string_view::npos; };
  if (tool.empty() || tool.find(' ') != std::string_view::npos || !clean(tool))
    return std::unexpected("invalid command name");
  std::string line = "/\x1e";  // fixed cwd prefix; see the header comment
  line += tool;
  for (const auto* group : {&argv, &literal}) {
    for (const std::string& argument : *group) {
      if (!clean(argument)) return std::unexpected("argument contains a reserved control byte");
      line += ' ';
      line += argument;
    }
  }
  return line;
}

bool resultIsUntrusted(Tier tier) { return tier == Tier::ObservePrivate; }

Decision evaluate(const RiskPolicy& policy, const GrantStore& grants, const TaintTracker& taint,
                  const std::string& client, std::string_view command, std::span<const std::string> args) {
  Decision decision;
  decision.tier = policy.tierFor(command, args);
  decision.tainted = taint.tainted(client);
  if (decision.tier == Tier::Blocked) {
    decision.reason = policy.knows(command) ? "blocked for agent clients" : "unknown command";
    return decision;
  }
  if (!grants.allows(client, decision.tier)) {
    decision.reason = "client has no " + std::string(tierName(decision.tier)) + " grant";
    return decision;
  }
  decision.verdict = decision.tier == Tier::Confirm ? Verdict::NeedsConfirmation : Verdict::Allow;
  return decision;
}

std::expected<void, std::string> AuditLog::append(const AuditRecord& record,
                                                  std::chrono::system_clock::time_point when) const {
  auto args = nlohmann::json::array();
  for (const auto& arg : record.args) args.push_back(truncated(arg, kMaxArgBytes));
  const nlohmann::json line{
    {"time", isoTime(when)}, {"client", record.client}, {"tool", record.tool}, {"args", std::move(args)},
    {"tier", std::string(tierName(record.tier))}, {"decision", record.decision}, {"outcome", record.outcome},
    {"tainted", record.tainted},
  };
  std::error_code ec;
  std::filesystem::create_directories(file_.parent_path(), ec);
  const int fd = ::open(file_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected(std::string("audit: ") + std::strerror(errno));
  const std::string text = line.dump() + "\n";  // dump() escapes control characters, so one record is one line
  const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  ::close(fd);
  if (!written) return std::unexpected("audit: short write");
  return {};
}

}  // namespace helix::agent
