#include "system/ai_usage.h"

#include <print>
#include <string>

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
    "seven_day": {"utilization": 40, "resets_at": "2026-09-20T00:00:00Z"}
  })", "Pro");
  ok = expect(claude && claude->status == Status::Ok && claude->windows.size() == 2, "claude windows") && ok;
  ok = expect(claude && claude->windows[0].key == WindowKey::Session && claude->windows[0].percent == 12.5F, "session pct")
      && ok;

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

  const auto copilot = parseCopilotUsageJson(R"({
    "copilot_plan": "pro",
    "quota_reset_date": "2026-10-01",
    "quota_snapshots": {
      "premium_interactions": {"percent_remaining": 80},
      "chat": {"percent_remaining": 50, "unlimited": false}
    }
  })");
  ok = expect(copilot && copilot->windows.size() == 2, "copilot windows") && ok;
  ok = expect(copilot && copilot->windows[0].percent == 20.0F, "copilot premium used") && ok;

  const auto cursor = parseCursorUsageJson(R"({
    "startOfMonth": "2026-09-01T00:00:00Z",
    "gpt-4": {"numRequests": 25, "maxRequestUsage": 100},
    "usageBasedPricing": {"numRequests": 3}
  })");
  ok = expect(cursor && cursor->status == Status::Ok && cursor->plan == "25/100", "cursor plan") && ok;
  ok = expect(cursor && cursor->windows.size() == 1 && cursor->windows[0].percent == 25.0F, "cursor pct") && ok;

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

  const auto ollama = parseOllamaSnapshot(
      R"({"models":[{"name":"llama3.2:latest","size":2019393189}]})",
      R"({"models":[{"name":"llama3.2:latest"}]})",
      R"({"version":"0.11.4"})"
  );
  ok = expect(ollama && ollama->version == "0.11.4" && ollama->models.size() == 1, "ollama models") && ok;
  ok = expect(ollama && ollama->models[0].loaded && ollama->models[0].size == 2019393189ULL, "ollama loaded") && ok;

  return ok ? 0 : 1;
}
