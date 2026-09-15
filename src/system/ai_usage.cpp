#include "system/ai_usage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <utility>

namespace noctalia::ai_usage {
  namespace {

    using json = nlohmann::json;

    [[nodiscard]] const json* objectField(const json& object, const char* key) {
      if (!object.is_object() || !object.contains(key)) {
        return nullptr;
      }
      const json& value = object[key];
      return value.is_object() ? &value : nullptr;
    }

    [[nodiscard]] std::string jsonString(const json& object, const char* key) {
      if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
        return {};
      }
      return object[key].get<std::string>();
    }

    [[nodiscard]] std::optional<double> jsonNumber(const json& object, const char* key) {
      if (!object.is_object() || !object.contains(key)) {
        return std::nullopt;
      }
      const json& value = object[key];
      if (value.is_number_float()) {
        return value.get<double>();
      }
      if (value.is_number_integer()) {
        return static_cast<double>(value.get<std::int64_t>());
      }
      if (value.is_number_unsigned()) {
        return static_cast<double>(value.get<std::uint64_t>());
      }
      return std::nullopt;
    }

    [[nodiscard]] bool jsonBool(const json& object, const char* key, bool fallback = false) {
      if (!object.is_object() || !object.contains(key) || !object[key].is_boolean()) {
        return fallback;
      }
      return object[key].get<bool>();
    }

    [[nodiscard]] std::optional<json> parseJson(std::string_view text) {
      try {
        return json::parse(text.begin(), text.end());
      } catch (const json::exception&) {
        return std::nullopt;
      }
    }

    [[nodiscard]] std::string capitalize(std::string value) {
      if (value.empty()) {
        return value;
      }
      value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
      return value;
    }

    [[nodiscard]] std::optional<std::string> decodeBase64Url(std::string_view input) {
      std::string padded(input);
      for (char& ch : padded) {
        if (ch == '-') {
          ch = '+';
        } else if (ch == '_') {
          ch = '/';
        }
      }
      while (padded.size() % 4 != 0) {
        padded.push_back('=');
      }
      static constexpr char kTable[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::array<int, 256> lookup{};
      lookup.fill(-1);
      for (int i = 0; i < 64; ++i) {
        lookup[static_cast<unsigned char>(kTable[i])] = i;
      }
      std::string out;
      out.reserve(padded.size());
      int buffer = 0;
      int bits = 0;
      for (unsigned char ch : padded) {
        if (ch == '=') {
          break;
        }
        const int value = lookup[ch];
        if (value < 0) {
          return std::nullopt;
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
      }
      return out;
    }

    [[nodiscard]] std::optional<Window> claudeWindow(const json& object, WindowKey key) {
      const auto utilization = jsonNumber(object, "utilization");
      if (!utilization.has_value()) {
        return std::nullopt;
      }
      Window window;
      window.key = key;
      window.percent = clampPercent(*utilization);
      window.reset = parseIsoUtc(jsonString(object, "resets_at"));
      return window;
    }

    [[nodiscard]] std::optional<Window> codexWindow(const json& object, std::int64_t nowEpoch) {
      if (!object.is_object()) {
        return std::nullopt;
      }
      Window window;
      const auto reset = jsonNumber(object, "resets_at");
      if (reset.has_value() && *reset > 0 && *reset <= static_cast<double>(nowEpoch)) {
        window.percent = 0.0F;
        window.reset = std::nullopt;
      } else {
        const auto used = jsonNumber(object, "used_percent");
        if (!used.has_value()) {
          return std::nullopt;
        }
        window.percent = clampPercent(*used);
        if (reset.has_value() && *reset > 0) {
          window.reset = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*reset));
        }
      }
      return window;
    }

  } // namespace

  std::string_view providerKey(ProviderId id) noexcept {
    switch (id) {
    case ProviderId::Claude:
      return "claude";
    case ProviderId::Codex:
      return "codex";
    case ProviderId::Gemini:
      return "gemini";
    case ProviderId::Qwen:
      return "qwen";
    case ProviderId::Copilot:
      return "copilot";
    case ProviderId::Cursor:
      return "cursor";
    case ProviderId::DeepSeek:
      return "deepseek";
    }
    return "claude";
  }

  std::string_view providerGlyph(ProviderId id) noexcept {
    switch (id) {
    case ProviderId::Claude:
      return "sparkles";
    case ProviderId::Codex:
      return "brand-openai";
    case ProviderId::Gemini:
      return "brand-google";
    case ProviderId::Qwen:
      return "robot";
    case ProviderId::Copilot:
      return "brand-github-copilot";
    case ProviderId::Cursor:
      return "pointer";
    case ProviderId::DeepSeek:
      return "fish";
    }
    return "sparkles";
  }

  std::optional<std::chrono::system_clock::time_point> parseIsoUtc(std::string_view iso) {
    if (iso.empty()) {
      return std::nullopt;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    const std::string text(iso);
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 3) {
      if (std::sscanf(text.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
        return std::nullopt;
      }
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;
    const std::time_t epoch = timegm(&tm);
    if (epoch == static_cast<std::time_t>(-1)) {
      return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(epoch);
  }

  float clampPercent(double value) noexcept {
    if (!std::isfinite(value)) {
      return 0.0F;
    }
    return static_cast<float>(std::clamp(value, 0.0, 100.0));
  }

  std::optional<ClaudeCredentials> parseClaudeCredentialsJson(std::string_view text) {
    const auto parsed = parseJson(text);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    const json* oauth = objectField(*parsed, "claudeAiOauth");
    if (oauth == nullptr) {
      return std::nullopt;
    }
    ClaudeCredentials creds;
    creds.accessToken = jsonString(*oauth, "accessToken");
    if (creds.accessToken.empty()) {
      return std::nullopt;
    }
    if (const auto expires = jsonNumber(*oauth, "expiresAt")) {
      creds.expiresAtMs = static_cast<std::int64_t>(*expires);
    }
    const std::string tier = jsonString(*oauth, "rateLimitTier");
    if (const auto pos = tier.find("max_"); pos != std::string::npos) {
      creds.plan = "Max " + tier.substr(pos + 4);
      if (!creds.plan.empty() && creds.plan.back() == 'x') {
        // keep "Max 5x"
      }
    } else {
      creds.plan = capitalize(jsonString(*oauth, "subscriptionType"));
    }
    return creds;
  }

  std::optional<ProviderSnapshot> parseClaudeUsageJson(std::string_view text, std::string plan) {
    const auto parsed = parseJson(text);
    if (!parsed.has_value() || !parsed->is_object()) {
      return std::nullopt;
    }
    ProviderSnapshot snap;
    snap.id = ProviderId::Claude;
    snap.status = Status::Ok;
    snap.plan = std::move(plan);
    if (const json* five = objectField(*parsed, "five_hour")) {
      if (auto window = claudeWindow(*five, WindowKey::Session)) {
        snap.windows.push_back(*window);
      }
    }
    if (const json* seven = objectField(*parsed, "seven_day")) {
      if (auto window = claudeWindow(*seven, WindowKey::Weekly)) {
        snap.windows.push_back(*window);
      }
    }
    if (snap.windows.empty()) {
      snap.status = Status::NoData;
      return snap;
    }
    return snap;
  }

  std::optional<ProviderSnapshot> parseCodexRateLimitsJson(std::string_view text, std::int64_t nowEpoch) {
    const auto parsed = parseJson(text);
    if (!parsed.has_value() || !parsed->is_object()) {
      return std::nullopt;
    }
    const json* payload = &*parsed;
    if (const json* nested = objectField(*parsed, "payload")) {
      payload = nested;
      if (const json* inner = objectField(*payload, "payload")) {
        payload = inner;
      }
    }
    const json* limits = objectField(*payload, "rate_limits");
    if (limits == nullptr) {
      return std::nullopt;
    }
    auto first = codexWindow((*limits)["primary"], nowEpoch);
    auto second = codexWindow((*limits)["secondary"], nowEpoch);
    const auto firstMinutes = jsonNumber((*limits)["primary"], "window_minutes").value_or(0);
    const auto secondMinutes = jsonNumber((*limits)["secondary"], "window_minutes").value_or(0);
    if (first.has_value() && second.has_value() && firstMinutes > secondMinutes) {
      std::swap(first, second);
    }
    ProviderSnapshot snap;
    snap.id = ProviderId::Codex;
    snap.status = Status::Ok;
    snap.plan = capitalize(jsonString(*limits, "plan_type"));
    if (first.has_value()) {
      first->key = WindowKey::Session;
      snap.windows.push_back(*first);
    }
    if (second.has_value()) {
      second->key = WindowKey::Weekly;
      snap.windows.push_back(*second);
    }
    if (snap.windows.empty()) {
      return std::nullopt;
    }
    if (parsed->contains("timestamp") && (*parsed)["timestamp"].is_string()) {
      snap.dataAt = parseIsoUtc((*parsed)["timestamp"].get<std::string>());
    }
    return snap;
  }

  std::optional<ProviderSnapshot> parseCopilotUsageJson(std::string_view text) {
    const auto parsed = parseJson(text);
    if (!parsed.has_value() || !parsed->is_object()) {
      return std::nullopt;
    }
    ProviderSnapshot snap;
    snap.id = ProviderId::Copilot;
    snap.status = Status::Ok;
    snap.plan = capitalize(jsonString(*parsed, "copilot_plan"));
    const auto reset = parseIsoUtc(jsonString(*parsed, "quota_reset_date"));
    if (const json* snapshots = objectField(*parsed, "quota_snapshots")) {
      if (const json* premium = objectField(*snapshots, "premium_interactions")) {
        if (const auto remaining = jsonNumber(*premium, "percent_remaining")) {
          Window window;
          window.key = WindowKey::Premium;
          window.percent = clampPercent(100.0 - *remaining);
          window.reset = reset;
          snap.windows.push_back(window);
        }
      }
      if (const json* chat = objectField(*snapshots, "chat")) {
        if (!jsonBool(*chat, "unlimited") && jsonNumber(*chat, "percent_remaining")) {
          Window window;
          window.key = WindowKey::Chat;
          window.percent = clampPercent(100.0 - *jsonNumber(*chat, "percent_remaining"));
          window.reset = reset;
          snap.windows.push_back(window);
        }
      }
    }
    if (snap.windows.empty()) {
      const json* limited = objectField(*parsed, "limited_user_quotas");
      const json* monthly = objectField(*parsed, "monthly_quotas");
      if (limited != nullptr && monthly != nullptr) {
        const auto freeReset = parseIsoUtc(jsonString(*parsed, "limited_user_reset_date"));
        const auto add = [&](const char* field, WindowKey key) {
          const auto left = jsonNumber(*limited, field);
          const auto max = jsonNumber(*monthly, field);
          if (!left.has_value() || !max.has_value() || *max <= 0) {
            return;
          }
          Window window;
          window.key = key;
          window.percent = clampPercent(((*max - *left) / *max) * 100.0);
          window.reset = freeReset;
          snap.windows.push_back(window);
        };
        add("chat", WindowKey::Chat);
        add("completions", WindowKey::Completions);
      }
    }
    if (snap.windows.empty()) {
      snap.status = Status::Unsupported;
    }
    return snap;
  }

  std::optional<ProviderSnapshot> parseCursorUsageJson(std::string_view text) {
    const auto parsed = parseJson(text);
    if (!parsed.has_value() || !parsed->is_object()) {
      return std::nullopt;
    }
    double bestPct = -1.0;
    double used = 0.0;
    double max = 0.0;
    for (auto it = parsed->begin(); it != parsed->end(); ++it) {
      if (!it.value().is_object()) {
        continue;
      }
      const auto cap = jsonNumber(it.value(), "maxRequestUsage");
      if (!cap.has_value() || *cap <= 0) {
        continue;
      }
      const auto requests = jsonNumber(it.value(), "numRequests").value_or(0);
      const double pct = (requests / *cap) * 100.0;
      if (pct > bestPct) {
        bestPct = pct;
        used = requests;
        max = *cap;
      }
    }
    if (bestPct < 0) {
      ProviderSnapshot snap;
      snap.id = ProviderId::Cursor;
      snap.status = Status::Unsupported;
      return snap;
    }
    ProviderSnapshot snap;
    snap.id = ProviderId::Cursor;
    snap.status = Status::Ok;
    snap.plan = std::to_string(static_cast<int>(used)) + "/" + std::to_string(static_cast<int>(max));
    Window window;
    window.key = WindowKey::Premium;
    window.percent = clampPercent(bestPct);
    if (const auto start = parseIsoUtc(jsonString(*parsed, "startOfMonth"))) {
      const std::time_t raw = std::chrono::system_clock::to_time_t(*start);
      std::tm tm{};
      gmtime_r(&raw, &tm);
      tm.tm_mon += 1;
      const std::time_t reset = timegm(&tm);
      if (reset != static_cast<std::time_t>(-1)) {
        window.reset = std::chrono::system_clock::from_time_t(reset);
      }
    }
    snap.windows.push_back(window);
    return snap;
  }

  std::optional<ProviderSnapshot> parseDeepseekBalanceJson(std::string_view text) {
    const auto parsed = parseJson(text);
    if (!parsed.has_value() || !parsed->is_object()) {
      return std::nullopt;
    }
    if (!parsed->contains("balance_infos") || !(*parsed)["balance_infos"].is_array()
        || (*parsed)["balance_infos"].empty()) {
      return std::nullopt;
    }
    const json& info = (*parsed)["balance_infos"].front();
    if (!info.is_object()) {
      return std::nullopt;
    }
    ProviderSnapshot snap;
    snap.id = ProviderId::DeepSeek;
    snap.status = Status::Ok;
    const std::string balance = info.contains("total_balance")
        ? (info["total_balance"].is_string() ? info["total_balance"].get<std::string>()
                                             : info["total_balance"].dump())
        : "?";
    snap.plan = balance + " " + jsonString(info, "currency");
    return snap;
  }

  std::optional<std::string> jwtSubject(std::string_view token) {
    const auto first = token.find('.');
    if (first == std::string_view::npos) {
      return std::nullopt;
    }
    const auto second = token.find('.', first + 1);
    if (second == std::string_view::npos) {
      return std::nullopt;
    }
    const auto decoded = decodeBase64Url(token.substr(first + 1, second - first - 1));
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    const auto parsed = parseJson(*decoded);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    const std::string sub = jsonString(*parsed, "sub");
    if (sub.empty()) {
      return std::nullopt;
    }
    return sub;
  }

  std::string cursorUserId(std::string_view subject) {
    const auto pipe = subject.find('|');
    if (pipe == std::string_view::npos) {
      return std::string(subject);
    }
    return std::string(subject.substr(pipe + 1));
  }

  int countPromptEvents(std::string_view raw, bool jsonl, std::string_view todayIso) {
    int count = 0;
    if (jsonl) {
      std::size_t start = 0;
      while (start < raw.size()) {
        auto end = raw.find('\n', start);
        if (end == std::string_view::npos) {
          end = raw.size();
        }
        const auto line = raw.substr(start, end - start);
        start = end + 1;
        if (line.empty()) {
          continue;
        }
        const auto parsed = parseJson(line);
        if (parsed.has_value() && parsed->is_object() && jsonString(*parsed, "type") == "user") {
          ++count;
        }
      }
      return count;
    }
    const auto parsed = parseJson(raw);
    if (!parsed.has_value() || !parsed->is_array()) {
      return 0;
    }
    for (const auto& entry : *parsed) {
      if (!entry.is_object() || jsonString(entry, "type") != "user") {
        continue;
      }
      const std::string stamp = jsonString(entry, "timestamp");
      if (stamp.size() >= todayIso.size() && stamp.substr(0, todayIso.size()) == todayIso) {
        ++count;
      }
    }
    return count;
  }

  std::span<const std::string_view> providerDesktopIds(ProviderId id) noexcept {
    switch (id) {
    case ProviderId::Claude: {
      static constexpr std::string_view kIds[] = {"claude", "Claude", "claude-code"};
      return kIds;
    }
    case ProviderId::Codex: {
      static constexpr std::string_view kIds[] = {"codex", "Codex"};
      return kIds;
    }
    case ProviderId::Gemini: {
      static constexpr std::string_view kIds[] = {"gemini", "com.google.gemini"};
      return kIds;
    }
    case ProviderId::Qwen: {
      static constexpr std::string_view kIds[] = {"qwen", "qwen-code"};
      return kIds;
    }
    case ProviderId::Copilot: {
      static constexpr std::string_view kIds[] = {"github-copilot"};
      return kIds;
    }
    case ProviderId::Cursor: {
      static constexpr std::string_view kIds[] = {"cursor", "Cursor", "com.todesktop.230313mzl4w4u92"};
      return kIds;
    }
    case ProviderId::DeepSeek:
      return {};
    }
    return {};
  }

  std::string_view providerCli(ProviderId id) noexcept {
    switch (id) {
    case ProviderId::Claude:
      return "claude";
    case ProviderId::Codex:
      return "codex";
    case ProviderId::Gemini:
      return "gemini";
    case ProviderId::Qwen:
      return "qwen";
    case ProviderId::Copilot:
      return {};
    case ProviderId::Cursor:
      return "cursor";
    case ProviderId::DeepSeek:
      return {};
    }
    return {};
  }

  bool providerOpensInTerminal(ProviderId id) noexcept { return id != ProviderId::Cursor; }

  std::string formatByteSize(std::uint64_t bytes) {
    if (bytes < 1024) {
      return std::to_string(bytes) + " B";
    }
    const char* units[] = {"KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit < 3) {
      value /= 1024.0;
      ++unit;
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), value >= 10.0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buf;
  }

  std::optional<OllamaSnapshot>
  parseOllamaSnapshot(std::string_view tagsJson, std::string_view psJson, std::string_view versionJson) {
    const auto tags = parseJson(tagsJson);
    if (!tags.has_value() || !tags->is_object() || !tags->contains("models") || !(*tags)["models"].is_array()) {
      return std::nullopt;
    }
    OllamaSnapshot snap;
    snap.status = OllamaStatus::Running;
    const auto version = parseJson(versionJson);
    if (version.has_value() && version->is_object()) {
      snap.version = jsonString(*version, "version");
    }
    std::vector<std::string> loaded;
    const auto ps = parseJson(psJson);
    if (ps.has_value() && ps->is_object() && ps->contains("models") && (*ps)["models"].is_array()) {
      for (const auto& entry : (*ps)["models"]) {
        if (!entry.is_object()) {
          continue;
        }
        std::string name = jsonString(entry, "name");
        if (name.empty()) {
          name = jsonString(entry, "model");
        }
        if (!name.empty()) {
          loaded.push_back(std::move(name));
        }
      }
    }
    for (const auto& entry : (*tags)["models"]) {
      if (!entry.is_object()) {
        continue;
      }
      OllamaModel model;
      model.name = jsonString(entry, "name");
      if (model.name.empty()) {
        model.name = jsonString(entry, "model");
      }
      if (model.name.empty()) {
        continue;
      }
      if (entry.contains("size") && entry["size"].is_number_unsigned()) {
        model.size = entry["size"].get<std::uint64_t>();
      } else if (entry.contains("size") && entry["size"].is_number_integer()) {
        const auto size = entry["size"].get<std::int64_t>();
        model.size = size > 0 ? static_cast<std::uint64_t>(size) : 0;
      }
      model.loaded = std::ranges::contains(loaded, model.name);
      snap.models.push_back(std::move(model));
    }
    return snap;
  }

} // namespace noctalia::ai_usage
