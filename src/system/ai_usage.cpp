#include "system/ai_usage.h"

#include "i18n/i18n.h"
#include "util/string_utils.h"

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
#include <unordered_set>
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

    [[nodiscard]] std::uint64_t jsonSize(const json& object, const char* key) noexcept {
      if (!object.is_object() || !object.contains(key)) {
        return 0;
      }
      const json& value = object[key];
      if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
      }
      if (value.is_number_integer()) {
        const auto size = value.get<std::int64_t>();
        return size > 0 ? static_cast<std::uint64_t>(size) : 0;
      }
      return 0;
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

    [[nodiscard]] bool nameMatchesInstalled(std::string_view installed, std::string_view wanted) noexcept {
      if (installed == wanted) {
        return true;
      }
      if (installed.size() > wanted.size() && installed.starts_with(wanted)
          && installed[wanted.size()] == ':') {
        return true;
      }
      if (wanted.size() > installed.size() && wanted.starts_with(installed)
          && wanted[installed.size()] == ':') {
        return true;
      }
      return false;
    }

    [[nodiscard]] std::string unescapeHtml(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
          out.push_back(text[i]);
          ++i;
          continue;
        }
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("&amp;")) {
          out.push_back('&');
          i += 5;
        } else if (rest.starts_with("&lt;")) {
          out.push_back('<');
          i += 4;
        } else if (rest.starts_with("&gt;")) {
          out.push_back('>');
          i += 4;
        } else if (rest.starts_with("&quot;")) {
          out.push_back('"');
          i += 6;
        } else if (rest.starts_with("&apos;")) {
          out.push_back('\'');
          i += 6;
        } else if (rest.starts_with("&#39;")) {
          out.push_back('\'');
          i += 5;
        } else if (rest.starts_with("&nbsp;")) {
          out.push_back(' ');
          i += 6;
        } else {
          out.push_back(text[i]);
          ++i;
        }
      }
      return out;
    }

    [[nodiscard]] std::string stripHtmlText(std::string_view html) {
      std::string out;
      out.reserve(html.size());
      bool inTag = false;
      for (char ch : html) {
        if (ch == '<') {
          inTag = true;
          continue;
        }
        if (ch == '>') {
          inTag = false;
          continue;
        }
        if (!inTag) {
          out.push_back(ch);
        }
      }
      return StringUtils::trim(unescapeHtml(out));
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

    [[nodiscard]] std::optional<double> jsonNumberAny(const json& object, std::initializer_list<const char*> keys) {
      for (const char* key : keys) {
        if (const auto value = jsonNumber(object, key)) {
          return value;
        }
      }
      return std::nullopt;
    }

    [[nodiscard]] std::string jsonStringAny(const json& object, std::initializer_list<const char*> keys) {
      for (const char* key : keys) {
        if (std::string value = jsonString(object, key); !value.empty()) {
          return value;
        }
      }
      return {};
    }

    [[nodiscard]] const json* objectFieldAny(const json& object, std::initializer_list<const char*> keys) {
      for (const char* key : keys) {
        if (const json* value = objectField(object, key)) {
          return value;
        }
      }
      return nullptr;
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
      std::optional<double> resetEpoch = jsonNumberAny(object, {"resets_at", "resetsAt"});
      if (!resetEpoch.has_value()) {
        if (const auto iso = parseIsoUtc(jsonStringAny(object, {"resets_at", "resetsAt"}))) {
          resetEpoch = static_cast<double>(std::chrono::system_clock::to_time_t(*iso));
        }
      }
      if (resetEpoch.has_value() && *resetEpoch > 0 && *resetEpoch <= static_cast<double>(nowEpoch)) {
        window.percent = 0.0F;
        window.reset = std::nullopt;
      } else {
        const auto used = jsonNumberAny(object, {"used_percent", "usedPercent"});
        if (!used.has_value()) {
          return std::nullopt;
        }
        window.percent = clampPercent(*used);
        if (resetEpoch.has_value() && *resetEpoch > 0) {
          window.reset = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*resetEpoch));
        }
      }
      return window;
    }

    [[nodiscard]] const json* codexLimitsObject(const json& parsed) {
      if (const json* nested = objectField(parsed, "payload")) {
        if (const json* inner = objectField(*nested, "payload")) {
          nested = inner;
        }
        if (const json* limits = objectFieldAny(*nested, {"rate_limits", "rateLimits"})) {
          return limits;
        }
      }
      if (const json* result = objectField(parsed, "result")) {
        if (const json* limits = objectFieldAny(*result, {"rate_limits", "rateLimits"})) {
          return limits;
        }
      }
      return objectFieldAny(parsed, {"rate_limits", "rateLimits"});
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

  std::string_view windowKeyId(WindowKey key) noexcept {
    switch (key) {
    case WindowKey::Session:
      return "session";
    case WindowKey::Weekly:
      return "weekly";
    case WindowKey::Day:
      return "day";
    case WindowKey::Premium:
      return "premium";
    case WindowKey::Chat:
      return "chat";
    case WindowKey::Completions:
      return "completions";
    case WindowKey::Opus:
      return "opus";
    case WindowKey::Sonnet:
      return "sonnet";
    case WindowKey::Extra:
      return "extra";
    }
    return "session";
  }

  std::string providerLabel(ProviderId id) {
    switch (id) {
    case ProviderId::Claude:
      return i18n::tr("control-center.ai-usage.providers.claude");
    case ProviderId::Codex:
      return i18n::tr("control-center.ai-usage.providers.codex");
    case ProviderId::Gemini:
      return i18n::tr("control-center.ai-usage.providers.gemini");
    case ProviderId::Qwen:
      return i18n::tr("control-center.ai-usage.providers.qwen");
    case ProviderId::Copilot:
      return i18n::tr("control-center.ai-usage.providers.copilot");
    case ProviderId::Cursor:
      return i18n::tr("control-center.ai-usage.providers.cursor");
    case ProviderId::DeepSeek:
      return i18n::tr("control-center.ai-usage.providers.deepseek");
    }
    return {};
  }

  std::string windowLabel(WindowKey key) {
    switch (key) {
    case WindowKey::Session:
      return i18n::tr("control-center.ai-usage.windows.session");
    case WindowKey::Weekly:
      return i18n::tr("control-center.ai-usage.windows.weekly");
    case WindowKey::Day:
      return i18n::tr("control-center.ai-usage.windows.day");
    case WindowKey::Premium:
      return i18n::tr("control-center.ai-usage.windows.premium");
    case WindowKey::Chat:
      return i18n::tr("control-center.ai-usage.windows.chat");
    case WindowKey::Completions:
      return i18n::tr("control-center.ai-usage.windows.completions");
    case WindowKey::Opus:
      return i18n::tr("control-center.ai-usage.windows.opus");
    case WindowKey::Sonnet:
      return i18n::tr("control-center.ai-usage.windows.sonnet");
    case WindowKey::Extra:
      return i18n::tr("control-center.ai-usage.windows.extra");
    }
    return {};
  }

  std::string ollamaStatusLabel(OllamaStatus status) {
    switch (status) {
    case OllamaStatus::Absent:
      return i18n::tr("control-center.ai-usage.ollama.absent");
    case OllamaStatus::Disabled:
      return i18n::tr("control-center.ai-usage.ollama.disabled");
    case OllamaStatus::Stopped:
      return i18n::tr("control-center.ai-usage.ollama.stopped");
    case OllamaStatus::Running:
      return i18n::tr("control-center.ai-usage.ollama.running");
    case OllamaStatus::Error:
      return i18n::tr("control-center.ai-usage.ollama.error");
    }
    return {};
  }

  std::string ollamaGlanceLabel(const OllamaSnapshot& ollama) {
    if (!ollama.loading.empty()) {
      return ollama.loading;
    }
    if (!ollama.deleting.empty()) {
      return ollama.deleting;
    }
    if (ollama.status == OllamaStatus::Running) {
      if (const std::string_view model = effectiveOllamaModel(ollama); !model.empty()) {
        return std::string(model);
      }
    }
    return ollamaStatusLabel(ollama.status);
  }

  const PullProgress* activeOllamaPull(const Snapshot& snapshot) noexcept {
    if (!snapshot.library.pull.has_value()) {
      return nullptr;
    }
    const auto& pull = *snapshot.library.pull;
    if (pull.done || pull.error || pull.name.empty()) {
      return nullptr;
    }
    return &pull;
  }

  const PullProgress* failedOllamaPull(const Snapshot& snapshot) noexcept {
    if (!snapshot.library.pull.has_value()) {
      return nullptr;
    }
    const auto& pull = *snapshot.library.pull;
    if (!pull.error || pull.name.empty()) {
      return nullptr;
    }
    return &pull;
  }

  std::string snapshotGlanceLabel(const Snapshot& snapshot) {
    if (const auto* pull = activeOllamaPull(snapshot)) {
      if (pull->total > 0) {
        char buf[16]{};
        std::snprintf(
            buf, sizeof(buf), "%.0f%%",
            100.0 * static_cast<double>(ollamaPullFraction(*pull))
        );
        return buf;
      }
      if (!pull->name.empty()) {
        return pull->name;
      }
    }
    return ollamaGlanceLabel(snapshot.ollama);
  }

  std::string_view effectiveOllamaModel(const OllamaSnapshot& ollama) noexcept {
    if (!ollama.selected.empty()) {
      return ollama.selected;
    }
    for (const auto& model : ollama.models) {
      if (model.loaded) {
        return model.name;
      }
    }
    return {};
  }

  bool ollamaModelIsLoaded(const OllamaSnapshot& ollama, std::string_view name) noexcept {
    if (name.empty()) {
      return false;
    }
    for (const auto& model : ollama.models) {
      if (model.name == name) {
        return model.loaded;
      }
    }
    return false;
  }

  std::uint64_t ollamaDiskBytes(const OllamaSnapshot& ollama) noexcept {
    std::uint64_t total = 0;
    for (const auto& model : ollama.models) {
      total += model.size;
    }
    return total;
  }

  std::uint64_t ollamaVramBytes(const OllamaSnapshot& ollama) noexcept {
    std::uint64_t total = 0;
    for (const auto& model : ollama.models) {
      if (model.loaded) {
        total += model.vram;
      }
    }
    return total;
  }

  bool ollamaModelMatchesQuery(const OllamaModel& model, std::string_view query) noexcept {
    const std::string needle = StringUtils::toLower(StringUtils::trim(query));
    if (needle.empty()) {
      return true;
    }
    auto has = [&](std::string_view text) {
      return StringUtils::toLower(text).find(needle) != std::string::npos;
    };
    return has(model.name) || has(model.parameterSize) || has(model.quantization);
  }

  void sortOllamaModels(std::vector<OllamaModel>& models, std::string_view selected) {
    std::ranges::stable_sort(models, [&](const OllamaModel& a, const OllamaModel& b) {
      const bool aSelected = !selected.empty() && a.name == selected;
      const bool bSelected = !selected.empty() && b.name == selected;
      if (aSelected != bSelected) {
        return aSelected;
      }
      if (a.loaded != b.loaded) {
        return a.loaded;
      }
      return a.name < b.name;
    });
  }

  std::string_view ollamaStatusKey(OllamaStatus status) noexcept {
    switch (status) {
    case OllamaStatus::Absent:
      return "absent";
    case OllamaStatus::Disabled:
      return "disabled";
    case OllamaStatus::Stopped:
      return "stopped";
    case OllamaStatus::Running:
      return "running";
    case OllamaStatus::Error:
      return "error";
    }
    return "unknown";
  }

  std::string_view localRuntimeKey(LocalRuntimeKind kind) noexcept {
    switch (kind) {
    case LocalRuntimeKind::LmStudio:
      return "lmstudio";
    case LocalRuntimeKind::LlamaCpp:
      return "llamacpp";
    case LocalRuntimeKind::Vllm:
      return "vllm";
    case LocalRuntimeKind::Jan:
      return "jan";
    }
    return "lmstudio";
  }

  std::string localRuntimeLabel(LocalRuntimeKind kind) {
    switch (kind) {
    case LocalRuntimeKind::LmStudio:
      return i18n::tr("control-center.ai-usage.local.lmstudio");
    case LocalRuntimeKind::LlamaCpp:
      return i18n::tr("control-center.ai-usage.local.llamacpp");
    case LocalRuntimeKind::Vllm:
      return i18n::tr("control-center.ai-usage.local.vllm");
    case LocalRuntimeKind::Jan:
      return i18n::tr("control-center.ai-usage.local.jan");
    }
    return {};
  }

  std::string_view localRuntimeGlyph(LocalRuntimeKind kind) noexcept {
    switch (kind) {
    case LocalRuntimeKind::LmStudio:
      return "cpu";
    case LocalRuntimeKind::LlamaCpp:
      return "terminal";
    case LocalRuntimeKind::Vllm:
      return "server";
    case LocalRuntimeKind::Jan:
      return "message-circle";
    }
    return "server";
  }

  std::uint16_t localRuntimePort(LocalRuntimeKind kind) noexcept {
    switch (kind) {
    case LocalRuntimeKind::LmStudio:
      return 1234;
    case LocalRuntimeKind::LlamaCpp:
      return 8080;
    case LocalRuntimeKind::Vllm:
      return 8000;
    case LocalRuntimeKind::Jan:
      return 1337;
    }
    return 0;
  }

  std::string_view statusKey(Status status) noexcept {
    switch (status) {
    case Status::Ok:
      return "ok";
    case Status::Absent:
      return "absent";
    case Status::Disabled:
      return "disabled";
    case Status::NoCredentials:
      return "no-credentials";
    case Status::NoAuth:
      return "no-auth";
    case Status::Expired:
      return "expired";
    case Status::NoData:
      return "no-data";
    case Status::HttpError:
      return "http-error";
    case Status::Unsupported:
      return "unsupported";
    }
    return "ok";
  }

  std::string formatSnapshotDump(const Snapshot& snapshot) {
    std::string out;
    out += snapshot.refreshing ? "refreshing true\n" : "refreshing false\n";
    for (const auto& provider : snapshot.providers) {
      if (!providerVisible(provider.status)) {
        continue;
      }
      out += providerKey(provider.id);
      out += ' ';
      out += statusKey(provider.status);
      if (!provider.plan.empty()) {
        out += " plan=";
        out += provider.plan;
      }
      for (const auto& window : provider.windows) {
        out += ' ';
        out += windowKeyId(window.key);
        out += '=';
        char pct[16]{};
        std::snprintf(pct, sizeof(pct), "%.0f", static_cast<double>(window.percent));
        out += pct;
        if (window.used.has_value() && window.max.has_value()) {
          out += '/';
          char used[32]{};
          char max[32]{};
          std::snprintf(used, sizeof(used), "%.0f", *window.used);
          std::snprintf(max, sizeof(max), "%.0f", *window.max);
          out += used;
          out += '/';
          out += max;
        }
      }
      if (provider.stale) {
        out += " stale";
      }
      out += '\n';
    }
    if (ollamaVisible(snapshot.ollama.status)) {
      out += "ollama ";
      out += ollamaStatusKey(snapshot.ollama.status);
      if (!snapshot.ollama.version.empty()) {
        out += " version=";
        out += snapshot.ollama.version;
      }
      std::size_t loaded = 0;
      for (const auto& model : snapshot.ollama.models) {
        if (model.loaded) {
          ++loaded;
        }
      }
      out += " models=";
      out += std::to_string(snapshot.ollama.models.size());
      out += " loaded=";
      out += std::to_string(loaded);
      if (!snapshot.ollama.selected.empty()) {
        out += " model=";
        out += snapshot.ollama.selected;
      }
      if (!snapshot.ollama.loading.empty()) {
        out += " loading=";
        out += snapshot.ollama.loading;
      }
      if (!snapshot.ollama.deleting.empty()) {
        out += " deleting=";
        out += snapshot.ollama.deleting;
      }
      if (snapshot.ollama.busy) {
        out += " busy";
      }
      out += '\n';
    }
    if (snapshot.library.searching || !snapshot.library.hits.empty() || snapshot.library.pull.has_value()
        || !snapshot.library.error.empty()) {
      out += "library hits=";
      out += std::to_string(snapshot.library.hits.size());
      if (!snapshot.library.query.empty()) {
        out += " query=";
        out += snapshot.library.query;
      }
      if (snapshot.library.searching) {
        out += " searching";
      }
      if (snapshot.library.pull.has_value()) {
        out += " pulling=";
        out += snapshot.library.pull->name;
        if (snapshot.library.pull->total > 0) {
          out += ' ';
          out += formatOllamaPullProgress(*snapshot.library.pull);
        }
      }
      if (!snapshot.library.error.empty()) {
        out += " error";
      }
      out += '\n';
    }
    for (const auto& runtime : snapshot.localRuntimes) {
      out += "local ";
      out += localRuntimeKey(runtime.kind);
      out += " running port=";
      out += std::to_string(runtime.port);
      out += " models=";
      out += std::to_string(runtime.models.size());
      out += '\n';
    }
    return out;
  }

  bool providerVisible(Status status) noexcept {
    return status != Status::Absent && status != Status::Disabled;
  }

  bool ollamaVisible(OllamaStatus status) noexcept {
    return status != OllamaStatus::Absent && status != OllamaStatus::Disabled;
  }

  bool localInferenceVisible(const Snapshot& snapshot) noexcept {
    return ollamaVisible(snapshot.ollama.status) || !snapshot.localRuntimes.empty();
  }

  const LocalRuntime* glanceableLocalRuntime(const Snapshot& snapshot) noexcept {
    if (snapshot.ollama.status == OllamaStatus::Running || snapshot.localRuntimes.empty()) {
      return nullptr;
    }
    return &snapshot.localRuntimes.front();
  }

  bool glanceableOllama(const Snapshot& snapshot) noexcept {
    return ollamaVisible(snapshot.ollama.status) && glanceableLocalRuntime(snapshot) == nullptr;
  }

  const Window* hottestWindow(const ProviderSnapshot& provider) noexcept {
    const Window* hottest = nullptr;
    for (const auto& window : provider.windows) {
      if (hottest == nullptr || window.percent > hottest->percent) {
        hottest = &window;
      }
    }
    return hottest;
  }

  std::optional<QuotaHit> hottestQuota(const Snapshot& snapshot) {
    std::optional<QuotaHit> best;
    for (const auto& provider : snapshot.providers) {
      if (provider.status != Status::Ok) {
        continue;
      }
      const Window* window = hottestWindow(provider);
      if (window == nullptr) {
        continue;
      }
      if (!best.has_value() || window->percent > best->window.percent) {
        best = QuotaHit{.provider = provider.id, .window = *window};
      }
    }
    return best;
  }

  std::string quotaWarningKey(ProviderId id, WindowKey key) {
    std::string out(providerKey(id));
    out.push_back(':');
    out += windowKeyId(key);
    return out;
  }

  std::vector<QuotaWarning> takeQuotaWarnings(
      const Snapshot& snapshot, int warnPercent, int warnResetMinutes,
      std::unordered_map<std::string, QuotaWarningState>& state, std::chrono::system_clock::time_point now
  ) {
    std::vector<QuotaWarning> warnings;
    std::unordered_set<std::string> seen;
    const float warnAt = static_cast<float>(warnPercent);
    const float clearHighBelow = static_cast<float>(std::max(0, warnPercent - 5));
    const auto resetHorizon =
        warnResetMinutes > 0 ? std::chrono::minutes(warnResetMinutes) : std::chrono::minutes(0);

    for (const auto& provider : snapshot.providers) {
      if (provider.status != Status::Ok) {
        continue;
      }
      for (const auto& window : provider.windows) {
        const std::string key = quotaWarningKey(provider.id, window.key);
        seen.insert(key);
        auto& flags = state[key];
        const bool highNow = warnPercent > 0 && window.percent + 0.5F >= warnAt;
        bool firedHigh = false;
        if (warnPercent <= 0) {
          flags.high = false;
        } else if (highNow && !flags.high) {
          warnings.push_back(
              QuotaWarning{.provider = provider.id, .window = window, .kind = QuotaWarningKind::High}
          );
          flags.high = true;
          firedHigh = true;
        } else if (!highNow && window.percent < clearHighBelow) {
          flags.high = false;
        }

        bool resetSoon = false;
        if (warnResetMinutes > 0 && window.reset.has_value()) {
          const auto remaining = *window.reset - now;
          resetSoon = remaining > std::chrono::seconds{0} && remaining <= resetHorizon && window.percent >= 50.0F;
        }
        if (resetSoon && !flags.reset) {
          flags.reset = true;
          if (!firedHigh) {
            warnings.push_back(
                QuotaWarning{.provider = provider.id, .window = window, .kind = QuotaWarningKind::ResetSoon}
            );
          }
        } else if (!resetSoon) {
          flags.reset = false;
        }
      }
    }
    std::erase_if(state, [&seen](const auto& entry) { return !seen.contains(entry.first); });
    return warnings;
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
    const auto addClaude = [&](const json* object, WindowKey key) {
      if (object == nullptr) {
        return;
      }
      auto window = claudeWindow(*object, key);
      if (!window.has_value()) {
        return;
      }
      if (key != WindowKey::Session && key != WindowKey::Weekly && window->percent <= 0.0F
          && !window->reset.has_value()) {
        return;
      }
      snap.windows.push_back(*window);
    };
    addClaude(objectField(*parsed, "five_hour"), WindowKey::Session);
    addClaude(objectField(*parsed, "seven_day"), WindowKey::Weekly);
    addClaude(objectField(*parsed, "seven_day_opus"), WindowKey::Opus);
    addClaude(objectField(*parsed, "seven_day_sonnet"), WindowKey::Sonnet);
    if (const json* extra = objectField(*parsed, "extra_usage"); extra != nullptr && jsonBool(*extra, "is_enabled")) {
      addClaude(extra, WindowKey::Extra);
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
    const json* limits = codexLimitsObject(*parsed);
    if (limits == nullptr) {
      return std::nullopt;
    }
    auto first = codexWindow((*limits)["primary"], nowEpoch);
    auto second = codexWindow((*limits)["secondary"], nowEpoch);
    const auto firstMinutes =
        jsonNumberAny((*limits)["primary"], {"window_minutes", "windowDurationMins"}).value_or(0);
    const auto secondMinutes =
        jsonNumberAny((*limits)["secondary"], {"window_minutes", "windowDurationMins"}).value_or(0);
    if (first.has_value() && second.has_value() && firstMinutes > secondMinutes) {
      std::swap(first, second);
    }
    ProviderSnapshot snap;
    snap.id = ProviderId::Codex;
    snap.status = Status::Ok;
    snap.plan = capitalize(jsonStringAny(*limits, {"plan_type", "planType"}));
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
      const auto addRemaining = [&](const json* object, WindowKey key, bool skipUnlimited) {
        if (object == nullptr) {
          return;
        }
        if (skipUnlimited && jsonBool(*object, "unlimited")) {
          return;
        }
        const auto remaining = jsonNumber(*object, "percent_remaining");
        if (!remaining.has_value()) {
          return;
        }
        Window window;
        window.key = key;
        window.percent = clampPercent(100.0 - *remaining);
        window.reset = reset;
        snap.windows.push_back(window);
      };
      addRemaining(objectField(*snapshots, "premium_interactions"), WindowKey::Premium, false);
      addRemaining(objectField(*snapshots, "chat"), WindowKey::Chat, true);
      addRemaining(objectField(*snapshots, "completions"), WindowKey::Completions, true);
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
          window.used = *max - *left;
          window.max = *max;
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
    window.used = used;
    window.max = max;
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

  std::span<const std::string_view> ollamaDesktopIds() noexcept {
    static constexpr std::string_view kIds[] = {"ollama", "Ollama", "com.ollama.ollama"};
    return kIds;
  }

  std::span<const std::string_view> localRuntimeDesktopIds(LocalRuntimeKind kind) noexcept {
    switch (kind) {
    case LocalRuntimeKind::LmStudio: {
      static constexpr std::string_view kIds[] = {"lm-studio", "com.lmstudio.LMStudio", "app.lmstudio.LMStudio"};
      return kIds;
    }
    case LocalRuntimeKind::Jan: {
      static constexpr std::string_view kIds[] = {"jan", "jan-app", "ai.jan.Jan"};
      return kIds;
    }
    case LocalRuntimeKind::LlamaCpp:
    case LocalRuntimeKind::Vllm:
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

  bool providerIsCli(ProviderId id) noexcept {
    switch (id) {
    case ProviderId::Claude:
    case ProviderId::Codex:
    case ProviderId::Gemini:
    case ProviderId::Qwen:
      return true;
    case ProviderId::Copilot:
    case ProviderId::Cursor:
    case ProviderId::DeepSeek:
      return false;
    }
    return false;
  }

  bool localRuntimeIsCli(LocalRuntimeKind kind) noexcept {
    switch (kind) {
    case LocalRuntimeKind::LlamaCpp:
    case LocalRuntimeKind::Vllm:
      return true;
    case LocalRuntimeKind::LmStudio:
    case LocalRuntimeKind::Jan:
      return false;
    }
    return false;
  }

  bool providerOpensInTerminal(ProviderId id) noexcept { return providerIsCli(id); }

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
    struct LoadedModel {
      std::string name;
      std::uint64_t vram = 0;
    };
    std::vector<LoadedModel> loaded;
    const auto ps = parseJson(psJson);
    if (ps.has_value() && ps->is_object() && ps->contains("models") && (*ps)["models"].is_array()) {
      for (const auto& entry : (*ps)["models"]) {
        if (!entry.is_object()) {
          continue;
        }
        LoadedModel running;
        running.name = jsonString(entry, "name");
        if (running.name.empty()) {
          running.name = jsonString(entry, "model");
        }
        if (running.name.empty()) {
          continue;
        }
        running.vram = jsonSize(entry, "size_vram");
        loaded.push_back(std::move(running));
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
      model.size = jsonSize(entry, "size");
      if (const json* details = objectField(entry, "details")) {
        model.parameterSize = jsonString(*details, "parameter_size");
        model.quantization = jsonString(*details, "quantization_level");
      }
      for (const auto& running : loaded) {
        if (running.name == model.name) {
          model.loaded = true;
          model.vram = running.vram;
          break;
        }
      }
      snap.models.push_back(std::move(model));
    }
    return snap;
  }

  std::optional<std::vector<std::string>> parseOpenAiModelsJson(std::string_view text) {
    const auto parsed = parseJson(text);
    if (!parsed.has_value() || !parsed->is_object() || !parsed->contains("data") || !(*parsed)["data"].is_array()) {
      return std::nullopt;
    }
    std::vector<std::string> ids;
    for (const auto& entry : (*parsed)["data"]) {
      if (!entry.is_object()) {
        continue;
      }
      std::string id = jsonString(entry, "id");
      if (id.empty()) {
        id = jsonString(entry, "name");
      }
      if (!id.empty()) {
        ids.push_back(std::move(id));
      }
    }
    return ids;
  }

  std::string ollamaKeepAliveJson(std::string_view model, int keepAlive) {
    nlohmann::json body;
    body["model"] = model;
    body["prompt"] = "";
    body["stream"] = false;
    body["keep_alive"] = keepAlive;
    return body.dump();
  }

  bool ollamaModelNameValid(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) {
      return false;
    }
    const auto ok = [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-' || ch == ':' || ch == '/';
    };
    if (std::isalnum(static_cast<unsigned char>(name.front())) == 0) {
      return false;
    }
    for (char ch : name) {
      if (!ok(static_cast<unsigned char>(ch))) {
        return false;
      }
    }
    const char last = name.back();
    if (last == '/' || last == ':' || last == '.' || last == '-') {
      return false;
    }
    return name.find("..") == std::string_view::npos && name.find("//") == std::string_view::npos;
  }

  bool ollamaModelInstalled(const OllamaSnapshot& ollama, std::string_view name) noexcept {
    return !ollamaInstalledName(ollama, name).empty();
  }

  std::string_view ollamaInstalledName(const OllamaSnapshot& ollama, std::string_view name) noexcept {
    if (name.empty()) {
      return {};
    }
    for (const auto& model : ollama.models) {
      if (nameMatchesInstalled(model.name, name)) {
        return model.name;
      }
    }
    return {};
  }

  std::string ollamaLibrarySearchUrl(std::string_view query) {
    const std::string trimmed = StringUtils::trim(query);
    if (trimmed.empty()) {
      return "https://ollama.com/library";
    }
    return "https://ollama.com/search?q=" + StringUtils::urlEncode(trimmed);
  }

  std::vector<LibraryHit> parseOllamaLibraryHtml(std::string_view html) {
    std::vector<LibraryHit> hits;
    constexpr std::string_view kHref = "href=\"/library/";
    std::size_t pos = 0;
    while ((pos = html.find(kHref, pos)) != std::string_view::npos) {
      pos += kHref.size();
      const std::size_t end = html.find('"', pos);
      if (end == std::string_view::npos) {
        break;
      }
      const std::string name(html.substr(pos, end - pos));
      pos = end + 1;
      if (!ollamaModelNameValid(name) || name.find('/') != std::string::npos) {
        continue;
      }
      bool seen = false;
      for (const auto& hit : hits) {
        if (hit.name == name) {
          seen = true;
          break;
        }
      }
      if (seen) {
        continue;
      }
      LibraryHit hit;
      hit.name = name;
      const std::size_t next = html.find(kHref, pos);
      const std::size_t cardEnd = next == std::string_view::npos ? html.size() : next;
      const std::size_t pStart = html.find("<p", pos);
      if (pStart != std::string_view::npos && pStart < cardEnd) {
        const std::size_t pOpen = html.find('>', pStart);
        const std::size_t pClose = html.find("</p>", pOpen == std::string_view::npos ? cardEnd : pOpen);
        if (pOpen != std::string_view::npos && pClose != std::string_view::npos && pClose < cardEnd) {
          hit.description = stripHtmlText(html.substr(pOpen + 1, pClose - pOpen - 1));
        }
      }
      const std::size_t pullsAt = html.find("&nbsp;Pulls", pos);
      if (pullsAt != std::string_view::npos && pullsAt < cardEnd) {
        const std::size_t spanClose = html.rfind("</span>", pullsAt);
        if (spanClose != std::string_view::npos && spanClose > pos) {
          const std::size_t spanOpen = html.rfind('>', spanClose);
          if (spanOpen != std::string_view::npos && spanOpen > pos && spanOpen < spanClose) {
            hit.pulls = stripHtmlText(html.substr(spanOpen + 1, spanClose - spanOpen - 1));
          }
        }
      }
      constexpr std::string_view kSizeChip = "bg-[#ddf4ff]";
      std::size_t chip = pos;
      while ((chip = html.find(kSizeChip, chip)) != std::string_view::npos && chip < cardEnd) {
        const std::size_t open = html.find('>', chip);
        const std::size_t close = html.find("</span>", open == std::string_view::npos ? cardEnd : open);
        chip = close == std::string_view::npos ? cardEnd : close + 7;
        if (open == std::string_view::npos || close == std::string_view::npos || close >= cardEnd) {
          break;
        }
        const std::string tag = stripHtmlText(html.substr(open + 1, close - open - 1));
        if (tag.empty()) {
          continue;
        }
        bool sizeLike = std::isdigit(static_cast<unsigned char>(tag.front())) != 0;
        if (!sizeLike) {
          continue;
        }
        if (!hit.sizes.empty()) {
          hit.sizes += " · ";
        }
        hit.sizes += tag;
      }
      hits.push_back(std::move(hit));
    }
    return hits;
  }

  std::optional<PullProgress> parseOllamaPullLine(std::string_view json) {
    const auto parsed = parseJson(json);
    if (!parsed.has_value() || !parsed->is_object()) {
      return std::nullopt;
    }
    PullProgress progress;
    progress.status = jsonString(*parsed, "status");
    const std::string error = jsonString(*parsed, "error");
    if (!error.empty()) {
      progress.error = true;
      progress.status = error;
    }
    if (const auto completed = jsonNumber(*parsed, "completed")) {
      progress.completed = *completed > 0 ? static_cast<std::uint64_t>(*completed) : 0;
    }
    if (const auto total = jsonNumber(*parsed, "total")) {
      progress.total = *total > 0 ? static_cast<std::uint64_t>(*total) : 0;
    }
    if (progress.status == "success") {
      progress.done = true;
    }
    progress.name = jsonStringAny(*parsed, {"model", "name"});
    if (progress.status.empty() && !progress.error) {
      return std::nullopt;
    }
    return progress;
  }

  std::string ollamaPullJson(std::string_view model) {
    nlohmann::json body;
    body["model"] = model;
    body["name"] = model;
    body["stream"] = true;
    return body.dump();
  }

  std::string ollamaDeleteJson(std::string_view model) {
    nlohmann::json body;
    body["model"] = model;
    body["name"] = model;
    return body.dump();
  }

  std::string formatOllamaPullProgress(const PullProgress& pull) {
    if (pull.error) {
      return pull.status;
    }
    if (pull.done) {
      return "success";
    }
    if (pull.total > 0) {
      const double pct = 100.0 * static_cast<double>(pull.completed) / static_cast<double>(pull.total);
      char buf[16]{};
      std::snprintf(buf, sizeof(buf), "%.0f%%", pct);
      return formatByteSize(pull.completed) + " / " + formatByteSize(pull.total) + " · " + buf;
    }
    return pull.status;
  }

  float ollamaPullFraction(const PullProgress& pull) noexcept {
    if (pull.total == 0) {
      return 0.0F;
    }
    return clampPercent(100.0 * static_cast<double>(pull.completed) / static_cast<double>(pull.total)) / 100.0F;
  }

} // namespace noctalia::ai_usage
