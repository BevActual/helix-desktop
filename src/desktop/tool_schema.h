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
nlohmann::json toolCallSchema(const noctalia::cli::Command& msgRoot, const RiskPolicy& policy, const std::set<Tier>& tiers);

}  // namespace helix::agent
