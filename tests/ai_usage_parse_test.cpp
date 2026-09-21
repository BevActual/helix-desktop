#include "system/ai_usage.h"

#include <chrono>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "ai_usage_parse_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using namespace noctalia::ai_usage;
  bool ok = true;

  const auto creds = parseClaudeCredentialsJson(R"({
    "claudeAiOauth": {
      "accessToken": "tok",
      "expiresAt": 4102444800000,
      "subscriptionType": "pro",
      "rateLimitTier": "default_claude_max_5x"
    }
  })");
  ok = expect(creds.has_value() && creds->accessToken == "tok", "claude token") && ok;
  ok = expect(creds && creds->plan == "Max 5x", "claude max plan") && ok;

  const auto claude = parseClaudeUsageJson(R"({
    "five_hour": {"utilization": 12.5, "resets_at": "2026-09-15T01:00:00Z"},
    "seven_day": {"utilization": 40, "resets_at": "2026-09-20T00:00:00Z"},
    "seven_day_opus": {"utilization": 8, "resets_at": "2026-09-20T00:00:00Z"},
    "seven_day_sonnet": {"utilization": 0},
    "extra_usage": {"is_enabled": true, "utilization": 11.0, "monthly_limit": 1700, "used_credits": 190}
  })", "Pro");
  ok = expect(claude && claude->status == Status::Ok && claude->windows.size() == 4, "claude windows") && ok;
  ok = expect(claude && claude->windows[0].key == WindowKey::Session && claude->windows[0].percent == 12.5F, "session pct")
      && ok;
  ok = expect(claude && claude->windows[2].key == WindowKey::Opus && claude->windows[2].percent == 8.0F, "opus pct")
      && ok;
  ok = expect(claude && claude->windows[3].key == WindowKey::Extra && claude->windows[3].percent == 11.0F, "extra pct")
      && ok;
  ok = expect(claude && !claude->windows[3].used.has_value(), "extra has no currency counts") && ok;

  const auto now = static_cast<std::int64_t>(1'800'000'000);
  const auto codex = parseCodexRateLimitsJson(R"({
    "timestamp": "2026-09-14T12:00:00Z",
    "payload": {
      "rate_limits": {
        "plan_type": "plus",
        "primary": {"used_percent": 10, "resets_at": 1900000000, "window_minutes": 300},
        "secondary": {"used_percent": 25, "resets_at": 1900000000, "window_minutes": 10080}
      }
    }
  })", now);
  ok = expect(codex && codex->windows.size() == 2 && codex->plan == "Plus", "codex") && ok;
  ok = expect(codex && codex->windows[0].key == WindowKey::Session, "codex session first") && ok;

  const auto liveNow = static_cast<std::int64_t>(1'700'000'000);
  const auto codexLive = parseCodexRateLimitsJson(R"({
    "id": 2,
    "result": {
      "rateLimits": {
        "planType": "plus",
        "primary": {"usedPercent": 10, "resetsAt": "2026-09-20T00:00:00Z", "windowDurationMins": 300},
        "secondary": {"usedPercent": 25, "resetsAt": "2026-09-27T00:00:00Z", "windowDurationMins": 10080}
      }
    }
  })", liveNow);
  ok = expect(codexLive && codexLive->windows.size() == 2 && codexLive->plan == "Plus", "codex app-server") && ok;
  ok = expect(codexLive && codexLive->windows[0].percent == 10.0F, "codex live session") && ok;

  const auto copilot = parseCopilotUsageJson(R"({
    "copilot_plan": "pro",
    "quota_reset_date": "2026-10-01",
    "quota_snapshots": {
      "premium_interactions": {"percent_remaining": 80},
      "chat": {"percent_remaining": 50, "unlimited": false},
      "completions": {"percent_remaining": 40, "unlimited": false}
    }
  })");
  ok = expect(copilot && copilot->windows.size() == 3, "copilot windows") && ok;
  ok = expect(copilot && copilot->windows[0].percent == 20.0F, "copilot premium used") && ok;
  ok = expect(copilot && copilot->windows[2].key == WindowKey::Completions && copilot->windows[2].percent == 60.0F, "copilot completions used") && ok;

  const auto cursor = parseCursorUsageJson(R"({
    "startOfMonth": "2026-09-01T00:00:00Z",
    "gpt-4": {"numRequests": 25, "maxRequestUsage": 100},
    "usageBasedPricing": {"numRequests": 3}
  })");
  ok = expect(cursor && cursor->status == Status::Ok && cursor->plan == "25/100", "cursor plan") && ok;
  ok = expect(cursor && cursor->windows.size() == 1 && cursor->windows[0].percent == 25.0F, "cursor pct") && ok;
  ok = expect(cursor && cursor->windows[0].used == 25.0 && cursor->windows[0].max == 100.0, "cursor used/max") && ok;

  const auto uncapped = parseCursorUsageJson(R"({"startOfMonth":"2026-09-01T00:00:00Z"})");
  ok = expect(uncapped && uncapped->status == Status::Unsupported, "cursor uncapped") && ok;

  const auto deepseek = parseDeepseekBalanceJson(R"({
    "balance_infos": [{"total_balance": "12.50", "currency": "USD"}]
  })");
  ok = expect(deepseek && deepseek->plan == "12.50 USD", "deepseek balance") && ok;

  ok = expect(countPromptEvents(R"({"type":"user"}
{"type":"assistant"}
{"type":"user"})", true, "2026-09-14") == 2, "jsonl prompts")
      && ok;
  ok = expect(
      countPromptEvents(
          R"([{"type":"user","timestamp":"2026-09-14T01:00:00Z"},{"type":"user","timestamp":"2026-09-13T01:00:00Z"}])",
          false, "2026-09-14"
      ) == 1,
      "logs.json today"
  ) && ok;

  const auto sub = jwtSubject("aaa.eyJzdWIiOiJhdXRoMHx1c2VyMTIzIn0.sig");
  ok = expect(sub.has_value() && *sub == "auth0|user123", "jwt sub") && ok;
  ok = expect(cursorUserId(*sub) == "user123", "cursor user id") && ok;

  const auto iso = parseIsoUtc("2026-09-15T01:02:03Z");
  ok = expect(iso.has_value(), "iso utc") && ok;

  ok = expect(formatByteSize(512) == "512 B", "bytes") && ok;
  ok = expect(formatByteSize(1536).find("KB") != std::string::npos, "kb") && ok;

  Snapshot snap;
  ProviderSnapshot claudeSnap;
  claudeSnap.id = ProviderId::Claude;
  claudeSnap.status = Status::Ok;
  claudeSnap.windows = {
      Window{.key = WindowKey::Session, .percent = 40.0F},
      Window{.key = WindowKey::Weekly, .percent = 72.0F},
  };
  ProviderSnapshot cursorSnap;
  cursorSnap.id = ProviderId::Cursor;
  cursorSnap.status = Status::Ok;
  cursorSnap.windows = {Window{.key = WindowKey::Premium, .percent = 91.0F}};
  snap.providers = {claudeSnap, cursorSnap};
  const auto hottest = hottestQuota(snap);
  ok = expect(hottest.has_value() && hottest->provider == ProviderId::Cursor, "hottest provider") && ok;
  ok = expect(hottest && hottest->window.percent == 91.0F, "hottest percent") && ok;

  std::unordered_map<std::string, QuotaWarningState> warningState;
  const auto clockNow = std::chrono::system_clock::now();
  auto high = takeQuotaWarnings(snap, 90, 0, warningState, clockNow);
  ok = expect(high.size() == 1 && high[0].kind == QuotaWarningKind::High, "high warning once") && ok;
  high = takeQuotaWarnings(snap, 90, 0, warningState, clockNow);
  ok = expect(high.empty(), "high warning does not repeat") && ok;
  cursorSnap.windows[0].percent = 88.0F;
  snap.providers[1] = cursorSnap;
  high = takeQuotaWarnings(snap, 90, 0, warningState, clockNow);
  ok = expect(high.empty(), "high warning hysteresis holds") && ok;
  cursorSnap.windows[0].percent = 80.0F;
  snap.providers[1] = cursorSnap;
  high = takeQuotaWarnings(snap, 90, 0, warningState, clockNow);
  ok = expect(high.empty(), "high warning clears below hysteresis") && ok;
  cursorSnap.windows[0].percent = 93.0F;
  snap.providers[1] = cursorSnap;
  high = takeQuotaWarnings(snap, 90, 0, warningState, clockNow);
  ok = expect(high.size() == 1, "high warning refires after drop") && ok;

  warningState.clear();
  claudeSnap.windows[1].percent = 60.0F;
  claudeSnap.windows[1].reset = clockNow + std::chrono::minutes(10);
  snap.providers = {claudeSnap};
  auto reset = takeQuotaWarnings(snap, 0, 30, warningState, clockNow);
  ok = expect(reset.size() == 1 && reset[0].kind == QuotaWarningKind::ResetSoon, "reset warning") && ok;
  reset = takeQuotaWarnings(snap, 0, 30, warningState, clockNow);
  ok = expect(reset.empty(), "reset warning does not repeat") && ok;

  const auto ollama = parseOllamaSnapshot(
      R"({"models":[{"name":"llama3.2:latest","size":2019393189,"details":{"parameter_size":"3.2B","quantization_level":"Q4_K_M"}}]})",
      R"({"models":[{"name":"llama3.2:latest","size_vram":1610612736}]})",
      R"({"version":"0.11.4"})"
  );
  ok = expect(ollama && ollama->version == "0.11.4" && ollama->models.size() == 1, "ollama models") && ok;
  ok = expect(ollama && ollama->models[0].loaded && ollama->models[0].size == 2019393189ULL, "ollama loaded") && ok;
  ok = expect(
           ollama && ollama->models[0].vram == 1610612736ULL && ollama->models[0].parameterSize == "3.2B"
               && ollama->models[0].quantization == "Q4_K_M",
           "ollama details"
       )
      && ok;
  ok = expect(ollama && ollamaDiskBytes(*ollama) == 2019393189ULL && ollamaVramBytes(*ollama) == 1610612736ULL, "ollama totals")
      && ok;
  ok = expect(ollama && ollamaModelMatchesQuery(ollama->models[0], "Q4") && !ollamaModelMatchesQuery(ollama->models[0], "70b"), "ollama query")
      && ok;

  Snapshot dump;
  dump.refreshing = true;
  ProviderSnapshot cursorDump;
  cursorDump.id = ProviderId::Cursor;
  cursorDump.status = Status::Ok;
  cursorDump.plan = "25/100";
  cursorDump.windows = {Window{.key = WindowKey::Premium, .percent = 25.0F, .used = 25.0, .max = 100.0}};
  dump.providers = {cursorDump};
  dump.ollama.status = OllamaStatus::Running;
  dump.ollama.version = "0.11.4";
  dump.ollama.models = {OllamaModel{.name = "llama", .loaded = true}};
  dump.ollama.selected = "llama";
  const auto text = formatSnapshotDump(dump);
  ok = expect(text.find("refreshing true") != std::string::npos, "dump refreshing") && ok;
  ok = expect(text.find("cursor ok plan=25/100 premium=25/25/100") != std::string::npos, "dump cursor") && ok;
  ok = expect(
           text.find("ollama running version=0.11.4 models=1 loaded=1 model=llama") != std::string::npos, "dump ollama"
       )
      && ok;
  dump.ollama.loading = "llama";
  ok = expect(formatSnapshotDump(dump).find(" loading=llama") != std::string::npos, "dump loading") && ok;
  dump.ollama.loading.clear();
  ok = expect(
           ollamaKeepAliveJson("llama3.2:latest", -1).find("\"keep_alive\":-1") != std::string::npos
               && ollamaKeepAliveJson("llama3.2:latest", -1).find("\"prompt\":\"\"") != std::string::npos,
           "keep-alive json"
       )
      && ok;

  dump.localRuntimes = {
      LocalRuntime{.kind = LocalRuntimeKind::LmStudio, .port = 1234, .models = {"qwen2.5"}},
  };
  const auto localDump = formatSnapshotDump(dump);
  ok = expect(localDump.find("local lmstudio running port=1234 models=1") != std::string::npos, "dump local") && ok;

  const auto models = parseOpenAiModelsJson(R"({"data":[{"id":"llama3"},{"name":"qwen"}]})");
  ok = expect(models && models->size() == 2 && (*models)[0] == "llama3" && (*models)[1] == "qwen", "openai models")
      && ok;
  ok = expect(!parseOpenAiModelsJson(R"(<html>8080</html>)").has_value(), "openai models rejects html") && ok;
  ok = expect(parseOpenAiModelsJson(R"({"data":[]})").has_value() && parseOpenAiModelsJson(R"({"data":[]})")->empty(), "openai models empty running")
      && ok;
  ok = expect(providerIsCli(ProviderId::Claude) && providerIsCli(ProviderId::Codex), "cli providers") && ok;
  ok = expect(!providerIsCli(ProviderId::Cursor) && !providerIsCli(ProviderId::Copilot), "gui providers") && ok;
  ok = expect(
           localRuntimeIsCli(LocalRuntimeKind::LlamaCpp) && !localRuntimeIsCli(LocalRuntimeKind::LmStudio), "cli local"
       )
      && ok;

  OllamaSnapshot glance;
  glance.status = OllamaStatus::Running;
  glance.selected = "llama3.2:latest";
  ok = expect(ollamaGlanceLabel(glance) == "llama3.2:latest", "glance selected") && ok;
  glance.loading = "qwen2.5";
  ok = expect(ollamaGlanceLabel(glance) == "qwen2.5", "glance loading") && ok;
  glance.loading.clear();
  glance.selected.clear();
  glance.models = {OllamaModel{.name = "phi", .loaded = true}};
  ok = expect(effectiveOllamaModel(glance) == "phi", "effective loaded") && ok;
  ok = expect(ollamaGlanceLabel(glance) == "phi", "glance loaded") && ok;

  std::vector<OllamaModel> ordered{
      {.name = "zeta", .loaded = false},
      {.name = "alpha", .loaded = true},
      {.name = "mid", .loaded = false},
  };
  sortOllamaModels(ordered, "mid");
  ok = expect(ordered.size() == 3 && ordered[0].name == "mid" && ordered[1].name == "alpha", "sort selected then loaded")
      && ok;

  ok = expect(ollamaModelNameValid("llama3.2") && ollamaModelNameValid("llama3.2:1b"), "model name valid") && ok;
  ok = expect(!ollamaModelNameValid("../x") && !ollamaModelNameValid(""), "model name rejects") && ok;
  ok = expect(
           ollamaLibrarySearchUrl("llama 3").find("search?q=llama%203") != std::string::npos
               && ollamaLibrarySearchUrl("  ") == "https://ollama.com/library",
           "library search url"
       )
      && ok;

  const auto library = parseOllamaLibraryHtml(R"(
<li><a href="/library/llama3.1" class="group w-full">
  <div class="flex flex-col mb-1" title="llama3.1">
    <h2><span>llama3.1</span></h2>
    <p class="max-w-lg">Llama 3.1 is a new state-of-the-art model from Meta&#39;s lineup.</p>
  </div>
  <span>119.7M</span>
  <span class="hidden sm:flex">&nbsp;Pulls</span>
  <span class="inline-flex bg-[#ddf4ff]">8b</span>
  <span class="inline-flex bg-[#ddf4ff]">70b</span>
</a></li>
<li><a href="/library/llama3.2" class="group w-full">
  <h2><span>llama3.2</span></h2>
  <p>Meta&amp;s small models.</p>
</a></li>
)");
  ok = expect(library.size() >= 2 && library[0].name == "llama3.1" && library[1].name == "llama3.2", "library names")
      && ok;
  ok = expect(library[0].description.find("Llama 3.1") != std::string::npos && library[0].description.find('\'') != std::string::npos, "library description")
      && ok;
  ok = expect(library[0].pulls == "119.7M", "library pulls") && ok;
  ok = expect(library[0].sizes.find("8b") != std::string::npos && library[0].sizes.find("70b") != std::string::npos, "library sizes")
      && ok;

  const auto pull = parseOllamaPullLine(R"({"status":"downloading","completed":50,"total":100,"model":"llama3.2"})");
  ok = expect(pull && pull->completed == 50 && pull->total == 100, "pull progress parsed") && ok;
  ok = expect(
           pull && formatOllamaPullProgress(*pull).find("50%") != std::string::npos
               && formatOllamaPullProgress(*pull).find(" / ") != std::string::npos,
           "pull progress bytes"
       )
      && ok;
  ok = expect(pull && ollamaPullFraction(*pull) == 0.5F, "pull fraction") && ok;
  ok = expect(ollamaDeleteJson("llama3.2").find("llama3.2") != std::string::npos, "delete json") && ok;
  const auto pullDone = parseOllamaPullLine(R"({"status":"success"})");
  ok = expect(pullDone && pullDone->done, "pull success") && ok;
  ok = expect(ollamaPullJson("llama3.2").find("\"stream\":true") != std::string::npos, "pull json") && ok;

  OllamaSnapshot installed;
  installed.models = {OllamaModel{.name = "llama3.2:latest"}};
  ok = expect(ollamaModelInstalled(installed, "llama3.2") && ollamaInstalledName(installed, "llama3.2") == "llama3.2:latest", "installed match")
      && ok;

  Snapshot dumpLib;
  dumpLib.library.hits = {LibraryHit{.name = "llama3.2"}};
  dumpLib.library.query = "llama";
  ok = expect(formatSnapshotDump(dumpLib).find("library hits=1 query=llama") != std::string::npos, "dump library")
      && ok;
  dumpLib.library.pull = PullProgress{.name = "llama3.2", .completed = 1, .total = 4};
  ok = expect(activeOllamaPull(dumpLib) != nullptr && snapshotGlanceLabel(dumpLib) == "25%", "glance pull") && ok;
  dumpLib.library.pull->error = true;
  dumpLib.library.pull->status = "disk full";
  ok = expect(activeOllamaPull(dumpLib) == nullptr && failedOllamaPull(dumpLib) != nullptr, "failed pull") && ok;
  dumpLib.library.pull.reset();
  dumpLib.ollama.status = OllamaStatus::Running;
  dumpLib.ollama.deleting = "phi";
  ok = expect(formatSnapshotDump(dumpLib).find("deleting=phi") != std::string::npos, "dump deleting") && ok;

  return ok ? 0 : 1;
}
