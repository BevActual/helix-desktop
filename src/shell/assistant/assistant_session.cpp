#include "shell/assistant/assistant_session.h"

#include "desktop_api/display_text.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace helix::assistant_ui {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxShown = 600;

std::string shown(const json& object, const char* key) {
  if (!object.contains(key) || !object.at(key).is_string()) return {};
  return helix::agent::sanitizeForDisplay(object.at(key).get<std::string>(), kMaxShown);
}

}  // namespace

void Session::set(View view) {
  view_ = std::move(view);
  if (changed_) changed_();
}

void Session::fail(const std::string& text) { set({.phase = Phase::Result, .text = text, .failed = true}); }

void Session::submit(const std::string& request) {
  if (view_.phase != Phase::Idle && view_.phase != Phase::Result) return;
  std::string trimmed = request;
  trimmed.erase(trimmed.begin(), std::ranges::find_if(trimmed, [](unsigned char c) { return !std::isspace(c); }));
  trimmed.erase(std::ranges::find_if(trimmed.rbegin(), trimmed.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), trimmed.end());
  if (trimmed.empty()) return;
  if (trimmed.size() > kMaxRequestChars) return fail("That is too long. Keep it under 500 characters.");

  request_ = trimmed;
  tool_.clear();
  args_.clear();
  const std::uint64_t generation = ++generation_;
  set({.phase = Phase::Thinking});
  const bool started = runner_ && runner_({"--json", request_}, [this, generation](int code, std::string output) { handlePlan(code, output, generation); });
  if (!started && generation == generation_) fail("The assistant is not installed (helix-assistant was not found).");
}

void Session::handlePlan(int /*exitCode*/, const std::string& output, std::uint64_t generation) {
  if (generation != generation_ || view_.phase != Phase::Thinking) return;  // cancelled or replaced meanwhile
  const json reply = json::parse(output.begin(), output.end(), nullptr, false);
  if (!reply.is_object() || !reply.contains("kind") || !reply.at("kind").is_string()) return fail("The assistant did not give an answer.");
  const std::string kind = reply.at("kind").get<std::string>();

  if (kind == "confirm") {
    // Remember exactly what is shown: this, and nothing else, is what a yes will run.
    if (!reply.contains("tool") || !reply.at("tool").is_string() || reply.at("tool").get<std::string>().empty() ||
        !reply.contains("args") || !reply.at("args").is_array()) return fail("The assistant gave an answer that could not be read.");
    std::vector<std::string> args;
    for (const auto& arg : reply.at("args")) {
      if (!arg.is_string()) return fail("The assistant gave an answer that could not be read.");
      args.push_back(arg.get<std::string>());
    }
    const std::string description = shown(reply, "description");
    if (description.empty()) return fail("The assistant gave an answer that could not be read.");
    tool_ = reply.at("tool").get<std::string>();
    args_ = std::move(args);
    set({.phase = Phase::Confirm, .description = description, .reason = shown(reply, "reason")});
    return;
  }
  if (kind == "done") return handleResult(output, generation);
  const std::string text = shown(reply, "text");
  fail(text.empty() ? "Something went wrong." : text);  // explain | error
}

void Session::confirm() {
  if (view_.phase != Phase::Confirm || tool_.empty()) return;
  const std::uint64_t generation = ++generation_;
  View acting = view_;
  acting.phase = Phase::Acting;
  set(std::move(acting));
  std::vector<std::string> arguments{"--json", "--request", request_, "--execute", tool_};
  arguments.insert(arguments.end(), args_.begin(), args_.end());
  tool_.clear();  // a second confirm cannot run it again
  args_.clear();
  const bool started = runner_ && runner_(std::move(arguments), [this, generation](int, std::string output) { handleResult(output, generation); });
  if (!started && generation == generation_) fail("The assistant is not installed (helix-assistant was not found).");
}

void Session::handleResult(const std::string& output, std::uint64_t generation) {
  if (generation != generation_ || (view_.phase != Phase::Acting && view_.phase != Phase::Thinking)) return;
  const json reply = json::parse(output.begin(), output.end(), nullptr, false);
  if (!reply.is_object() || !reply.contains("kind") || !reply.at("kind").is_string()) return fail("The assistant did not give an answer.");
  const std::string kind = reply.at("kind").get<std::string>();
  const std::string text = shown(reply, "text");
  if (kind == "done") {
    set({.phase = Phase::Result, .text = text.empty() ? "Done." : text, .unverified = reply.value("untrusted", false)});
  } else {
    fail(text.empty() ? "Something went wrong." : text);
  }
}

void Session::cancel() {
  ++generation_;
  tool_.clear();
  args_.clear();
  set({});
}

}  // namespace helix::assistant_ui
