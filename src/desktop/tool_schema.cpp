#include "desktop/tool_schema.h"

#include <algorithm>

namespace helix::agent {

namespace {

using nlohmann::json;

constexpr int kMaxArgChars = 500;  // bounds free text so a model cannot run on forever

json stringSchema(std::span<const std::string_view> choices, const std::set<std::string>* allowed = nullptr) {
  if (choices.empty() && allowed == nullptr) return {{"type", "string"}, {"maxLength", kMaxArgChars}};
  json values = json::array();
  for (const std::string_view choice : choices)
    if (allowed == nullptr || allowed->contains(std::string(choice))) values.push_back(std::string(choice));
  if (choices.empty() && allowed != nullptr)
    for (const auto& choice : *allowed) values.push_back(choice);
  return {{"type", "string"}, {"enum", std::move(values)}};
}

// `allowedFirst`, when set, limits the first positional (or the subcommand name) to those choices.
json callSchema(const noctalia::cli::Command& command, const std::set<std::string>* allowedFirst) {
  json prefix = json::array();
  std::size_t required = 0;
  bool open = false;  // accepts any number of further strings

  if (!command.subcommands.empty()) {
    // `color-scheme-set builtin <name>`: the subcommand name, then its own positionals as plain strings.
    std::set<std::string> names;
    for (const auto& sub : command.subcommands) names.insert(std::string(sub.name));
    prefix.push_back(stringSchema({}, allowedFirst ? allowedFirst : &names));
    required = 1;
    open = true;
  } else {
    for (std::size_t i = 0; i < command.positionals.size(); ++i) {
      const auto& positional = command.positionals[i];
      prefix.push_back(stringSchema(positional.choices, i == 0 ? allowedFirst : nullptr));
      if (positional.required) required = i + 1;
      if (positional.variadic || positional.joinRemaining) open = true;
    }
  }

  json args = {{"type", "array"}, {"minItems", required}};
  if (!prefix.empty()) args["prefixItems"] = prefix;
  if (open) args["items"] = stringSchema({});
  else args["items"] = false;  // nothing beyond the declared positionals
  if (!open) args["maxItems"] = prefix.size();

  return {
    {"type", "object"},
    {"properties", {{"tool", {{"const", std::string(command.name)}}}, {"args", std::move(args)},
                    {"reason", {{"type", "string"}, {"maxLength", 200}}}}},
    {"required", json::array({"tool", "args"})},
    {"additionalProperties", false},
  };
}

}  // namespace

json toolCallSchema(const noctalia::cli::Command& msgRoot, const RiskPolicy& policy, const std::set<Tier>& tiers) {
  json alternatives = json::array();
  for (const auto& command : msgRoot.subcommands) {
    const std::string name(command.name);
    if (!policy.knows(name)) continue;  // an unclassified command is blocked, so it is never offered
    if (tiers.contains(policy.tierFor(name))) {
      alternatives.push_back(callSchema(command, nullptr));
      continue;
    }
    std::set<std::string> granted;
    for (const auto& [choice, tier] : policy.choiceTiers(name))
      if (tiers.contains(tier)) granted.insert(choice);
    if (!granted.empty()) alternatives.push_back(callSchema(command, &granted));
  }
  return {
    {"$schema", "https://json-schema.org/draft/2020-12/schema"},
    {"title", "Helix desktop tool call"},
    {"oneOf", std::move(alternatives)},
  };
}

}  // namespace helix::agent
