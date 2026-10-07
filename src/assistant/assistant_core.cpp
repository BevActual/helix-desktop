#include "assistant/assistant_core.h"

namespace helix::assistant {

using nlohmann::json;

namespace {

// The five examples measured in the evaluation (tests/eval/desktop_requests.json), plus one decline so the format is known.
struct Example { const char* request; const char* reply; };
constexpr Example kExamples[] = {
  {"Set the log level to debug.", R"({"tool":"log-level-set","args":["debug"]})"},
  {"Make the bar hide itself unless I move the mouse to the edge.", R"({"tool":"bar-auto-hide-set","args":["smart"]})"},
  {"Reload the configuration.", R"({"tool":"config-reload","args":[]})"},
  {"Turn the on-screen popups off.", R"({"tool":"osd-disable","args":[]})"},
  {"Set the output audio profile to Music.", R"({"tool":"effects-profile-set","args":["output","Music"]})"},
};

}  // namespace

Reply parseReply(std::string_view text) {
  Reply reply;
  const json value = json::parse(text.begin(), text.end(), nullptr, false);
  if (!value.is_object() || !value.contains("tool") || !value.at("tool").is_string()) {
    reply.error = "the model's reply was not a tool call";
    return reply;
  }
  reply.tool = value.at("tool").get<std::string>();
  if (value.contains("reason") && value.at("reason").is_string()) reply.reason = value.at("reason").get<std::string>();
  if (value.contains("args")) {
    if (!value.at("args").is_array()) { reply.error = "arguments were not a list"; return reply; }
    for (const auto& arg : value.at("args")) {
      if (!arg.is_string()) { reply.error = "an argument was not text"; return reply; }
      reply.args.push_back(arg.get<std::string>());
    }
  }
  reply.kind = reply.tool == agent::kDeclineTool ? Reply::Kind::Decline : Reply::Kind::Call;
  if (reply.kind == Reply::Kind::Decline) reply.args.clear();  // a decline has no arguments to forward, whatever was sent
  return reply;
}

json buildMessages(std::string_view catalog, std::string_view request) {
  json messages = json::array({{{"role", "system"}, {"content", "You control a Linux desktop. Answer with exactly one JSON tool call for the user's request, "
                                "choosing the single most suitable tool. Use only these tools:\n" + std::string(catalog)}}});
  for (const auto& example : kExamples) {
    messages.push_back({{"role", "user"}, {"content", example.request}});
    messages.push_back({{"role", "assistant"}, {"content", example.reply}});
  }
  messages.push_back({{"role", "user"}, {"content", std::string(request)}});
  return messages;
}

Step stepFor(const agent::RiskPolicy& policy, const std::set<agent::Tier>& granted, const Reply& reply) {
  if (reply.kind != Reply::Kind::Call) return Step::Explain;
  if (!policy.knows(reply.tool)) return Step::Refuse;
  const agent::Tier tier = policy.tierFor(reply.tool, reply.args);
  if (!granted.contains(tier)) return Step::Refuse;  // also covers `blocked`, which is never granted
  switch (tier) {
    case agent::Tier::Observe:
    case agent::Tier::ObservePrivate:
      return Step::Run;
    default:
      return Step::AskFirst;
  }
}

std::string describe(const Reply& reply, const agent::ToolHints* hints) {
  std::string call = reply.tool;
  for (const auto& arg : reply.args) call += " " + arg;
  const std::string* hint = hints ? hints->find(reply.tool) : nullptr;
  return hint ? *hint + " (`" + call + "`)" : "Run `" + call + "`";
}

ModelChoice chooseModel(const json& models, std::string_view preferred) {
  if (!models.is_array() || models.empty()) return {"", "no model is installed (put one in ~/.local/share/helix/models)"};
  const json* best = nullptr;
  for (const auto& model : models) {
    if (!model.is_object() || !model.contains("id") || !model.at("id").is_string()) continue;
    if (!preferred.empty() && model.at("id").get<std::string>() == preferred) return {std::string(preferred), ""};
    if (!best || model.value("bytes", std::uint64_t{0}) > best->value("bytes", std::uint64_t{0})) best = &model;
  }
  if (!preferred.empty()) return {"", "the model '" + std::string(preferred) + "' is not installed"};
  if (!best) return {"", "no usable model is installed"};
  return {best->at("id").get<std::string>(), ""};
}

}  // namespace helix::assistant
