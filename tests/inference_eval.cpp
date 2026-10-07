// Evaluation of the tool-calling assistant against a real model: how often it picks the right desktop tool.
//
//   inference_eval --root <repo> --model <file.gguf> [--cases file] [--strategy flat|staged] [--decline 0|1]
//                  [--fewshot N] [--out report.json] [--limit N]
//
// Every reply is constrained to the tool-call grammar, so validity is guaranteed and only measured as a sanity check; the
// numbers that matter are whether the right tool was chosen, whether arguments were right, and whether requests nothing
// can satisfy were declined. Cases and accepted answers live in tests/eval/desktop_requests.json.
#include "cli/schema_msg.h"
#include "desktop/tool_schema.h"
#include "inference/inference_queue.h"
#include "inference/llama_backend.h"
#include "tests/schema_validator.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <print>
#include <set>

using namespace helix;
using nlohmann::json;

namespace {

struct Options {
  std::filesystem::path root, model, cases, out;
  std::string strategy = "flat";
  bool decline = false;
  int fewshot = 0;
  int limit = 0;
  bool reasonFirst = false;
};

std::string groupOf(const std::string& tool) { return tool.substr(0, tool.find('-')); }

bool sameArgs(const json& accepted, const json& got) { return accepted.is_null() || accepted == got; }

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--root") opt.root = value;
    else if (key == "--model") opt.model = value;
    else if (key == "--cases") opt.cases = value;
    else if (key == "--strategy") opt.strategy = value;
    else if (key == "--decline") opt.decline = value == "1";
    else if (key == "--fewshot") opt.fewshot = std::stoi(value);
    else if (key == "--out") opt.out = value;
    else if (key == "--limit") opt.limit = std::stoi(value);
    else if (key == "--reason-first") opt.reasonFirst = value == "1";
    else { std::println(stderr, "unknown option {}", key); return 2; }
  }
  if (opt.root.empty() || !std::filesystem::exists(opt.model)) { std::println(stderr, "need --root and an existing --model"); return 2; }
  if (opt.cases.empty()) opt.cases = opt.root / "tests/eval/desktop_requests.json";

  const json data = json::parse(std::ifstream(opt.cases));
  auto cases = data.at("cases");
  const auto examples = data.at("examples");
  if (opt.limit > 0 && static_cast<std::size_t>(opt.limit) < cases.size()) cases.erase(cases.begin() + opt.limit, cases.end());

  auto policy = agent::RiskPolicy::load(opt.root / "protocols/ipc/msg-risk.toml");
  if (!policy) { std::println(stderr, "{}", policy.error()); return 2; }
  const std::set<agent::Tier> tiers{agent::Tier::Observe, agent::Tier::ObservePrivate, agent::Tier::Act};
  const auto& root = noctalia::cli::kMsgCmd;
  const json schema = agent::toolCallSchema(root, *policy, tiers, nullptr, opt.decline);

  auto backend = std::make_shared<inference::LlamaBackend>(inference::LlamaBackend::Config{.modelDir = opt.model.parent_path()});
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::uint64_t, inference::Outcome> done;
  inference::InferenceQueue queue(backend, {}, [](std::uint64_t, const std::string&) {},
      [&](std::uint64_t op, const inference::Outcome& outcome) { { std::lock_guard lock(mutex); done[op] = outcome; } changed.notify_all(); });

  auto ask = [&](const json& messages, const std::string& jsonSchema, std::uint32_t maxTokens) {
    inference::ChatRequest request;
    request.client = "eval";
    request.model = opt.model.filename().string();
    request.messages = messages;
    request.jsonSchema = jsonSchema;
    request.reasonFirst = opt.reasonFirst && !jsonSchema.empty();
    request.maxTokens = maxTokens;
    const auto op = queue.submit(request);
    if (!op) { std::println(stderr, "submit refused: {}", op.error()); std::exit(2); }
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(300), [&] { return done.contains(*op); })) { std::println(stderr, "timed out"); std::exit(2); }
    return done[*op];
  };

  // Prompts. Few-shot examples are earlier turns of the conversation, so the model sees the exact output format.
  auto buildMessages = [&](const std::string& catalog, const std::string& request) {
    json messages = json::array({{{"role", "system"}, {"content", "You control a Linux desktop. Answer with exactly one JSON tool call for the user's request, "
                                  "choosing the single most suitable tool. Use only these tools:\n" + catalog}}});
    for (int i = 0; i < opt.fewshot && i < static_cast<int>(examples.size()); ++i) {
      if (!opt.decline && examples[i].at("call").at("tool") == "decline") continue;
      messages.push_back({{"role", "user"}, {"content", examples[i].at("request")}});
      messages.push_back({{"role", "assistant"}, {"content", examples[i].at("call").dump()}});
    }
    messages.push_back({{"role", "user"}, {"content", request}});
    return messages;
  };

  std::map<std::string, std::vector<std::string>> groups;  // staged strategy: tool-name prefix -> tools
  std::string groupSchema, groupSystem;
  if (opt.strategy == "staged") {
    for (const auto& alternative : schema.at("oneOf")) {
      const auto tool = alternative.at("properties").at("tool").at("const").get<std::string>();
      groups[groupOf(tool)].push_back(tool);
    }
    json alternatives = json::array();
    std::string catalog;
    for (const auto& [group, tools] : groups) {
      alternatives.push_back({{"type", "object"}, {"properties", {{"tool", {{"const", group}}}, {"args", {{"type", "array"}, {"minItems", 0}, {"items", false}}}}}});
      std::string summary;
      for (std::size_t i = 0; i < tools.size() && i < 3; ++i) summary += (i ? ", " : "") + tools[i];
      catalog += "- " + group + ": " + summary + (tools.size() > 3 ? ", ..." : "") + "\n";
    }
    groupSchema = json{{"oneOf", alternatives}}.dump();
    groupSystem = "You control a Linux desktop. Pick the group of tools that best fits the user's request"
                  + std::string(opt.decline ? ", or decline if none fits" : "") + ". Answer with a JSON object whose \"tool\" is the group name. Groups:\n" + catalog;
  }
  const std::string flatCatalog = agent::toolCatalog(root, *policy, tiers, nullptr, opt.decline);
  const std::string flatSchema = schema.dump();

  // The first request pays for loading the model and compiling GPU kernels; it is not what a user waits for in steady state.
  (void)ask(json::array({{{"role", "user"}, {"content", "Reply with OK."}}}), "", 4);

  json report = {{"model", opt.model.filename().string()}, {"strategy", opt.strategy}, {"decline", opt.decline}, {"fewshot", opt.fewshot}, {"reason_first", opt.reasonFirst}, {"results", json::array()}};
  std::map<std::string, std::map<std::string, int>> byCategory;
  std::map<std::string, int> total;
  std::vector<long> latencies;
  std::uint64_t promptTotal = 0, cachedTotal = 0;
  for (const auto& c : cases) {
    const std::string request = c.at("request");
    const bool mustDecline = c.value("expect", "") == "decline";
    const auto started = std::chrono::steady_clock::now();
    std::string group;
    inference::Outcome outcome;
    if (opt.strategy == "staged") {
      json groupMessages = json::array({{{"role", "system"}, {"content", groupSystem}}, {{"role", "user"}, {"content", request}}});
      const auto stage = ask(groupMessages, groupSchema, 64);
      try { group = json::parse(stage.text).at("tool").get<std::string>(); } catch (const std::exception&) {}
      const std::set<std::string> shortlist(groups[group].begin(), groups[group].end());
      const bool haveGroup = !shortlist.empty();
      std::set<std::string> only = shortlist;
      only.erase(agent::kDeclineTool);
      const json narrowed = agent::toolCallSchema(root, *policy, tiers, haveGroup ? &only : nullptr, opt.decline && (!haveGroup || group == agent::kDeclineTool));
      outcome = ask(buildMessages(agent::toolCatalog(root, *policy, tiers, haveGroup ? &only : nullptr, opt.decline && (!haveGroup || group == agent::kDeclineTool)), request), narrowed.dump(), 128);
    } else {
      outcome = ask(buildMessages(flatCatalog, request), flatSchema, 128);
    }
    const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    latencies.push_back(ms);
    promptTotal += outcome.promptTokens;
    cachedTotal += outcome.cachedPromptTokens;

    json reply;
    bool valid = false;
    try { reply = json::parse(outcome.text); valid = helix::test::valid(schema, reply); } catch (const std::exception&) {}
    std::string kind;
    const std::string tool = valid ? reply.at("tool").get<std::string>() : "";
    if (!valid) kind = "invalid";
    else if (mustDecline) kind = tool == agent::kDeclineTool ? "declined-correctly" : "acted-instead-of-declining";
    else if (tool == agent::kDeclineTool) kind = "declined-wrongly";
    else {
      kind = "wrong-tool";
      for (const auto& accepted : c.at("accept")) {
        if (accepted.at("tool") != tool) continue;
        kind = sameArgs(accepted.at("args"), reply.at("args")) ? "exact" : "right-tool-wrong-args";
        if (kind == "exact") break;
      }
    }
    ++total[kind];
    ++byCategory[c.at("category")][kind];
    ++byCategory[c.at("category")]["_n"];
    report["results"].push_back({{"id", c.at("id")}, {"category", c.at("category")}, {"request", request}, {"kind", kind}, {"reply", outcome.text}, {"group", group}, {"ms", ms}});
    std::println("  {:27s} {:44s} -> {}", kind, request, outcome.text.substr(0, 90));
  }

  const int n = static_cast<int>(cases.size());
  int actionable = 0, declineCases = 0;
  for (const auto& c : cases) (c.value("expect", "") == "decline" ? declineCases : actionable)++;
  std::ranges::sort(latencies);
  const long median = latencies.empty() ? 0 : latencies[latencies.size() / 2];
  const long p95 = latencies.empty() ? 0 : latencies[std::min(latencies.size() - 1, latencies.size() * 95 / 100)];
  const int exact = total["exact"], toolOnly = total["right-tool-wrong-args"];
  report["summary"] = {{"cases", n}, {"actionable", actionable}, {"must_decline", declineCases}, {"counts", total},
    {"exact_rate", actionable ? double(exact) / actionable : 0.0}, {"right_tool_rate", actionable ? double(exact + toolOnly) / actionable : 0.0},
    {"decline_recall", declineCases ? double(total["declined-correctly"]) / declineCases : 0.0},
    {"median_ms", median}, {"p95_ms", p95}, {"prompt_tokens", promptTotal}, {"cached_prompt_tokens", cachedTotal}};
  report["by_category"] = byCategory;
  std::println("\n{} / {} / decline={} / fewshot={}: {} cases", opt.model.filename().string(), opt.strategy, opt.decline, opt.fewshot, n);
  std::println("  exact {}/{} ({:.0f}%)  right tool {}/{} ({:.0f}%)  declined correctly {}/{}  wrongly declined {}  acted instead of declining {}  invalid {}",
               exact, actionable, actionable ? 100.0 * exact / actionable : 0.0, exact + toolOnly, actionable,
               actionable ? 100.0 * (exact + toolOnly) / actionable : 0.0, total["declined-correctly"], declineCases,
               total["declined-wrongly"], total["acted-instead-of-declining"], total["invalid"]);
  std::println("  latency per request: median {} ms, p95 {} ms; {:.0f}% of prompt tokens served from the cache", median, p95, promptTotal ? 100.0 * cachedTotal / promptTotal : 0.0);
  if (!opt.out.empty()) std::ofstream(opt.out) << report.dump(1) << "\n";
  return total["invalid"] == 0 ? 0 : 1;  // validity is guaranteed by the grammar; anything else is a bug, not a score
}
