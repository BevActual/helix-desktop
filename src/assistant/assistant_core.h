#pragma once

#include "desktop/agent_policy.h"
#include "desktop/tool_schema.h"

#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The decisions of the Helix assistant that need no D-Bus: reading the model's reply and deciding what happens to it.
// Everything a model says is a suggestion. The Agent Surface service still decides what is allowed; this only decides what the
// assistant shows the person first. Design: docs/design/agent-and-inference.md.
namespace helix::assistant {

struct Reply {
  enum class Kind { Call, Decline, Invalid } kind = Kind::Invalid;
  std::string tool;
  std::vector<std::string> args;
  std::string reason;  // the model's own words: why it declined, or why it chose this tool
  std::string error;   // Kind::Invalid only
};

// `text` is the model's constrained reply: {"tool": ..., "args": [...], "reason"?: ...}. `decline` is a reply, never a call.
Reply parseReply(std::string_view text);

// The conversation sent to the model: instructions and tool list, the few-shot examples that were measured, then the request.
nlohmann::json buildMessages(std::string_view catalog, std::string_view request);

// What to do with a parsed reply.
enum class Step {
  Run,       // read-only: run it and show the answer
  AskFirst,  // changes something: show "about to do X" and wait for a yes
  Refuse,    // not something this client may do (unknown tool, blocked tier, no grant)
  Explain,   // the model declined, or the reply was unusable: show why, do nothing
};
Step stepFor(const agent::RiskPolicy& policy, const std::set<agent::Tier>& granted, const Reply& reply);

// What a person is shown before saying yes: the tool's plain-words description when there is one, then the exact call, e.g.
//   Step to the next power profile (`power-cycle next`)
// The description matters: `power-cycle` sounds like a restart, and the person must not be misled by a tool's name.
std::string describe(const Reply& reply, const agent::ToolHints* hints = nullptr);

struct ModelChoice { std::string id; std::string error; };
// Which installed model to ask. `preferred` (the person's choice, HELIX_ASSISTANT_MODEL) wins if installed. Otherwise the
// largest, which is the one the measurements favour (7B: 88% held-out vs 1.5B far lower). `models` is ListModels' reply.
ModelChoice chooseModel(const nlohmann::json& models, std::string_view preferred);

}  // namespace helix::assistant
