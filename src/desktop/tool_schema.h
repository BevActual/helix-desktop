#pragma once

#include "cli/schema.h"
#include "desktop/agent_policy.h"

#include <nlohmann/json.hpp>
#include <set>

namespace helix::agent {

// A JSON Schema (2020-12) of every desktop tool call a client may make with the given tiers:
//   {"tool": "<msg command>", "args": [<positional arguments>], "reason": "<why>"}
// Built from the msg command schema and the risk policy, the same two sources the service enforces, so the schema and
// the service cannot drift apart. A small local model constrained by it (structured output / grammar) can only name
// real commands, pass the right number of arguments and pick valid enum values. It narrows what a model can say; the
// service still decides what is allowed.
//
// Commands outside `tiers` are absent. A command that is blocked as a whole but has granted choices (`session lock`)
// appears with its first argument limited to those choices.
//
// `onlyTools`, when given, further limits the result to those tool names (a shortlist for the request at hand).
//
// `includeDecline` adds a `decline` tool ({"tool":"decline","args":[],"reason":"<why>"}, reason required). A grammar that
// forces a call leaves a model no way to say "none of these fit", so a request nothing can satisfy would be answered with
// a nearby tool. `decline` is not a desktop command: whoever receives it must not forward it, only show the reason.
inline constexpr const char* kDeclineTool = "decline";
nlohmann::json toolCallSchema(const noctalia::cli::Command& msgRoot, const RiskPolicy& policy, const std::set<Tier>& tiers,
                              const std::set<std::string>* onlyTools = nullptr, bool includeDecline = false);

// A compact, one-line-per-tool description of the same set of tools, for a model's system prompt:
//   panel-open <id> [context] - Open a panel by id, optionally with context
//   media <action: next|previous|toggle|...> - Control active media playback
// Only tools toolCallSchema offers for `tiers` are listed, so the prompt and the grammar always agree.
std::string toolCatalog(const noctalia::cli::Command& msgRoot, const RiskPolicy& policy, const std::set<Tier>& tiers,
                        const std::set<std::string>* onlyTools = nullptr, bool includeDecline = false);

}  // namespace helix::agent
