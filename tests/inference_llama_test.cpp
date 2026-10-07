// End to end with a real model: the llama.cpp backend, the queue, the grammar and the tool schema together.
// Skipped (exit 77) when the model file is not there. Usage: inference_llama_test <repo-root> <model.gguf>
#include "cli/schema_msg.h"
#include "desktop/tool_schema.h"
#include "inference/inference_queue.h"
#include "inference/llama_backend.h"
#include "tests/schema_validator.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <unistd.h>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

using namespace helix;
using nlohmann::json;

namespace {

struct Waiter {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::uint64_t, inference::Outcome> done;
  std::map<std::uint64_t, int> deltaCount;

  inference::Outcome wait(std::uint64_t op, int seconds = 120) {
    std::unique_lock lock(mutex);
    TEST_CHECK(changed.wait_for(lock, std::chrono::seconds(seconds), [&] { return done.contains(op); }));
    return done[op];
  }
};

struct Case { const char* request; const char* tool; json args; };  // args null = any

// Two-stage selection: first the group a tool belongs to (its name up to the first hyphen), then the tool within it.
std::string groupOf(const std::string& tool) { return tool.substr(0, tool.find('-')); }

}  // namespace

// Which files count as models, and what may be loaded, without loading anything.
void checkModelListing() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("helix-model-list-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  auto make = [&](const std::string& name, std::size_t bytes) { std::ofstream(dir / name) << std::string(bytes, 'x'); };
  make("small.gguf", 10);
  make("big-00001-of-00002.gguf", 100);
  make("big-00002-of-00002.gguf", 50);
  make("broken-00001-of-00003.gguf", 7);       // a set with a shard missing is not offered
  make("broken-00003-of-00003.gguf", 7);
  make("notes.txt", 5);
  inference::LlamaBackend backend({.modelDir = dir});
  const auto models = backend.models();
  TEST_CHECK(models.size() == 2);
  TEST_CHECK(models[0].id == "big-00001-of-00002.gguf" && models[0].bytes == 150);  // listed once, with the combined size
  TEST_CHECK(models[1].id == "small.gguf" && models[1].bytes == 10);
  std::string error;
  for (const char* refused : {"big-00002-of-00002.gguf", "broken-00001-of-00003.gguf", "../etc/passwd.gguf", "/etc/passwd", "notes.txt", "missing.gguf"}) {
    TEST_CHECK(!backend.load(refused, error));
    TEST_CHECK(error.find("no such model") != std::string::npos);
  }
  fs::remove_all(dir);
}

int main(int argc, char** argv) {
  checkModelListing();
  if (argc < 3 || !std::filesystem::exists(argv[2])) { std::println(stderr, "model not found: skipping"); return 77; }
  const std::filesystem::path model = argv[2];

  auto policy = agent::RiskPolicy::load(std::filesystem::path(argv[1]) / "protocols/ipc/msg-risk.toml");
  TEST_CHECK(policy.has_value());
  const std::set<agent::Tier> tiers{agent::Tier::Observe, agent::Tier::ObservePrivate, agent::Tier::Act};
  const json schema = agent::toolCallSchema(noctalia::cli::kMsgCmd, *policy, tiers);
  const std::string catalog = agent::toolCatalog(noctalia::cli::kMsgCmd, *policy, tiers);

  auto backend = std::make_shared<inference::LlamaBackend>(inference::LlamaBackend::Config{.modelDir = model.parent_path()});
  Waiter waiter;
  inference::InferenceQueue queue(backend, {},
      [&](std::uint64_t op, const std::string& piece) {
        static const auto t0 = std::chrono::steady_clock::now();
        if (std::getenv("HELIX_TEST_TRACE"))
          std::println(stderr, "      +{:>6} ms op{} {:?}", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(), op, piece);
        std::lock_guard lock(waiter.mutex);
        ++waiter.deltaCount[op];
      },
      [&](std::uint64_t op, const inference::Outcome& outcome) {
        { std::lock_guard lock(waiter.mutex); waiter.done[op] = outcome; }
        waiter.changed.notify_all();
      });

  const auto models = backend->models();
  TEST_CHECK(std::ranges::any_of(models, [&](const inference::ModelInfo& m) { return m.id == model.filename().string(); }));
  std::println("model: {}", model.filename().string());

  auto ask = [&](const std::string& user, const std::string& system, const std::string& jsonSchema, std::uint32_t maxTokens) {
    inference::ChatRequest request;
    request.client = "test";
    request.model = model.filename().string();
    request.messages = json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}});
    request.jsonSchema = jsonSchema;
    request.maxTokens = maxTokens;
    const auto started = std::chrono::steady_clock::now();
    const auto op = queue.submit(request);
    TEST_CHECK(op.has_value());
    auto outcome = waiter.wait(*op);
    if (std::getenv("HELIX_TEST_TIMING"))
      std::println(stderr, "    ({} ms, {} prompt tokens, {} generated)",
                   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
                   outcome.promptTokens, outcome.completionTokens);
    return std::pair{*op, std::move(outcome)};
  };

  // 1. Plain text works, streams, and ends on its own.
  {
    const auto [op, outcome] = ask("Reply with exactly the single word OK.", "You are terse.", "", 8);
    std::println("free text: ok={} text={:?} tokens={}", outcome.ok, outcome.text, outcome.completionTokens);
    TEST_CHECK(outcome.ok && outcome.text.find("OK") != std::string::npos);
    TEST_CHECK(waiter.deltaCount[op] > 0);
  }

  // 2. Structured output: every reply must be a valid tool call. Whether it is the RIGHT one is measured, not assumed.
  const std::string system = "You control a Linux desktop. Answer with exactly one JSON tool call for the user's request, "
                             "choosing the single most suitable tool. Use only these tools:\n" + catalog;
  const std::vector<Case> cases = {
    {"Open the control center.", "panel-open", json::array({"control-center"})},
    {"What is the current status of the desktop?", "status", json::array()},
    {"Make it louder.", "volume-up", nullptr},
    {"Switch to dark mode.", "theme-mode-set", json::array({"dark"})},
    {"Hide the top bar.", "bar-hide", nullptr},
    {"Go to the next workspace.", "workspace-switch", json::array({"next"})},
    {"Pause the music.", "media", json::array({"pause"})},
    {"What is on my clipboard?", "clipboard-text", json::array()},
    {"Turn on do not disturb.", "notification-dnd-set", json::array({"on"})},
    {"Set the volume to 30 percent.", "volume-set", json::array({"30"})},
    {"Lock the screen.", "session", json::array({"lock"})},
    {"Take a screenshot of the whole screen.", "screenshot-fullscreen", nullptr},
    {"Mute the speakers.", "volume-mute", nullptr},
    {"Show me my notification history.", "notification-history", json::array()},
  };
  if (const char* limit = std::getenv("HELIX_TEST_CASES"); limit && std::atoi(limit) > 0)
    const_cast<std::vector<Case>&>(cases).resize(std::min<std::size_t>(cases.size(), static_cast<std::size_t>(std::atoi(limit))));
  int validCount = 0, rightTool = 0, exact = 0;
  const std::string schemaText = schema.dump();
  for (const auto& c : cases) {
    const auto [op, outcome] = ask(c.request, system, schemaText, 96);
    json reply;
    bool valid = false;
    try { reply = json::parse(outcome.text); valid = helix::test::valid(schema, reply); } catch (const std::exception&) {}
    const bool tool = valid && reply.at("tool") == c.tool;
    const bool args = tool && (c.args.is_null() || reply.at("args") == c.args);
    validCount += valid; rightTool += tool; exact += args;
    std::println("  [{}{}] {:44s} -> {}", valid ? "valid" : "INVALID", args ? ", exact" : (tool ? ", tool" : ""), c.request, outcome.text);
    TEST_CHECK(outcome.ok);
    TEST_CHECK(valid);  // the grammar guarantees this: a failure here is a bug in the grammar or the sampler
  }
  std::println("structured output: valid {}/{}, right tool {}/{}, exact {}/{}", validCount, cases.size(), rightTool, cases.size(), exact, cases.size());
  TEST_CHECK(validCount == static_cast<int>(cases.size()));
  // Accuracy is the model's job and is reported above. This floor only says "not broken": a 1.5B model picks the right tool
  // about half the time from a flat list of ~120 tools, which is why the real assistant needs a better selection step or a
  // larger model (see docs/design/agent-and-inference.md).
  TEST_CHECK(rightTool >= 4);

  // 2b. The same requests, in two stages: the model picks a group (about 45 choices), then a tool from that group only.
  {
    std::map<std::string, std::vector<std::string>> groups;  // group -> tools, in schema order
    for (const auto& alternative : schema.at("oneOf")) {
      const auto tool = alternative.at("properties").at("tool").at("const").get<std::string>();
      groups[groupOf(tool)].push_back(tool);
    }
    json groupAlternatives = json::array();
    std::string groupCatalog;
    for (const auto& [group, tools] : groups) {
      groupAlternatives.push_back({{"type", "object"},
        {"properties", {{"tool", {{"const", group}}}, {"args", {{"type", "array"}, {"minItems", 0}, {"items", false}}}}}});
      std::string summary;  // a few of the group's tool names tell the model what the group is for
      for (std::size_t i = 0; i < tools.size() && i < 3; ++i) summary += (i ? ", " : "") + tools[i];
      groupCatalog += "- " + group + ": " + summary + (tools.size() > 3 ? ", ..." : "") + "\n";
    }
    const std::string groupSchema = json{{"oneOf", groupAlternatives}}.dump();
    const std::string groupSystem = "You control a Linux desktop. Pick the group of tools that best fits the user's request. "
                                    "Answer with a JSON object whose \"tool\" is the group name. Groups:\n" + groupCatalog;
    int stagedValid = 0, stagedRight = 0, stagedExact = 0;
    for (const auto& c : cases) {
      const auto [gop, groupOutcome] = ask(c.request, groupSystem, groupSchema, 64);
      std::string group;
      try { group = json::parse(groupOutcome.text).at("tool").get<std::string>(); } catch (const std::exception&) {}
      // If the group stage fails, fall back to the whole list instead of asking for a call from an empty one.
      const std::set<std::string> shortlist(groups[group].begin(), groups[group].end());
      const bool haveGroup = !shortlist.empty();
      const json narrowed = agent::toolCallSchema(noctalia::cli::kMsgCmd, *policy, tiers, haveGroup ? &shortlist : nullptr);
      const auto [op, outcome] = ask(c.request, "You control a Linux desktop. Answer with exactly one JSON tool call for the user's request. "
                                                "Use only these tools:\n" + agent::toolCatalog(noctalia::cli::kMsgCmd, *policy, tiers, haveGroup ? &shortlist : nullptr),
                                     narrowed.dump(), 96);
      json reply;
      bool valid = false;
      try { reply = json::parse(outcome.text); valid = helix::test::valid(schema, reply); } catch (const std::exception&) {}
      const bool tool = valid && reply.at("tool") == c.tool;
      const bool args = tool && (c.args.is_null() || reply.at("args") == c.args);
      stagedValid += valid; stagedRight += tool; stagedExact += args;
      std::println("  [{}{}] {:44s} group={:12s} -> {}", valid ? "valid" : "INVALID", args ? ", exact" : (tool ? ", tool" : ""), c.request, group, outcome.text);
    }
    std::println("two-stage: valid {}/{}, right tool {}/{}, exact {}/{}  (single-stage was {}/{} right tool)", stagedValid, cases.size(),
                 stagedRight, cases.size(), stagedExact, cases.size(), rightTool, cases.size());
    TEST_CHECK(stagedValid == static_cast<int>(cases.size()));
  }

  // 2c. A constrained reply that runs out of tokens half-way is an error, never a silently truncated "answer".
  {
    const auto [op, outcome] = ask("Open the control center.", system, schemaText, 3);
    std::println("cut off at 3 tokens: ok={} truncated={} error={:?} text={:?}", outcome.ok, outcome.truncated, outcome.error, outcome.text);
    TEST_CHECK(!outcome.ok && outcome.truncated && outcome.error.find("cut off") != std::string::npos);
  }

  // 2d. The prompt cache: the shared start of the prompt (the tool list) is not processed again, and, the property that
  // matters, a cached run gives exactly the same answer as a cold one.
  {
    using namespace std::chrono_literals;
    const auto [op1, first] = ask("Pause the music.", system, schemaText, 96);
    const auto [op2, other] = ask("Make it louder.", system, schemaText, 96);
    const auto [op3, again] = ask("Pause the music.", system, schemaText, 96);
    std::println("cache: prompt {} tokens; second request reused {} ({:.0f}%), repeat reused {}",
                 other.promptTokens, other.cachedPromptTokens, 100.0 * other.cachedPromptTokens / other.promptTokens, again.cachedPromptTokens);
    TEST_CHECK(first.ok && other.ok && again.ok);
    TEST_CHECK(other.cachedPromptTokens * 10 >= other.promptTokens * 8);   // at least 80% of a different request's prompt was shared
    TEST_CHECK(again.cachedPromptTokens * 10 >= again.promptTokens * 8);   // the cache holds the latest request: the shared start is reused
    TEST_CHECK(again.text == first.text);                                  // reuse never changes the answer
    const auto [op3b, repeat] = ask("Pause the music.", system, schemaText, 96);
    TEST_CHECK(repeat.cachedPromptTokens + 1 >= repeat.promptTokens);      // straight after itself: all but the last token
    TEST_CHECK(repeat.text == first.text);

    TEST_CHECK(queue.unloadIfIdle(0s));                                    // free the model and its memory entirely
    const auto [op4, cold] = ask("Pause the music.", system, schemaText, 96);
    std::println("cold run after unload: reused {} tokens", cold.cachedPromptTokens);
    TEST_CHECK(cold.cachedPromptTokens == 0 && cold.text == first.text);   // a fresh context agrees with the cached runs
  }

  // 3. Cancelling stops generation early and keeps what was produced.
  {
    inference::ChatRequest request;
    request.client = "test";
    request.messages = json::array({{{"role", "user"}, {"content", "Count from 1 to 300, separated by commas."}}});
    request.maxTokens = 512;
    const auto op = *queue.submit(request);
    while (true) { std::lock_guard lock(waiter.mutex); if (waiter.deltaCount[op] >= 5 || waiter.done.contains(op)) break; }
    queue.cancel("test", op);
    const auto outcome = waiter.wait(op);
    std::println("cancelled after {} pieces: cancelled={} chars={}", waiter.deltaCount[op], outcome.cancelled, outcome.text.size());
    TEST_CHECK(outcome.cancelled && !outcome.text.empty() && outcome.completionTokens < 200);
  }
  return 0;
}
