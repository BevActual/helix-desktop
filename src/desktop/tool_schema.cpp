#include "desktop/tool_schema.h"

#include "core/toml.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace helix::agent {

  namespace {

    using nlohmann::json;

    constexpr int kMaxArgChars = 500; // bounds free text so a model cannot run on forever

    // An optional free-text argument (a bar name, a monitor, a connector) is something a model copies from the
    // signature it is shown, as a placeholder, and when it precedes a required one the command's own parsing does not
    // need it at all. So only required arguments and enums are offered, plus the optional ones that are really
    // meaningful.
    bool meaningfulOptional(const std::string& command, std::string_view positional) {
      static const std::set<std::pair<std::string, std::string>> kept{
          {"panel-open", "context"},
          {"panel-toggle", "context"},
          {"settings-open", "context"},
          {"settings-toggle", "context"}
      };
      return kept.contains({command, std::string(positional)});
    }

    std::vector<const helix::cli::Positional*> offeredPositionals(const helix::cli::Command& command) {
      std::vector<const helix::cli::Positional*> out;
      for (const auto& positional : command.positionals)
        if (positional.required
            || !positional.choices.empty()
            || meaningfulOptional(std::string(command.name), positional.name))
          out.push_back(&positional);
      return out;
    }

    json stringSchema(std::span<const std::string_view> choices, const std::set<std::string>* allowed = nullptr) {
      if (choices.empty() && allowed == nullptr)
        return {{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxArgChars}};
      json values = json::array();
      for (const std::string_view choice : choices)
        if (allowed == nullptr || allowed->contains(std::string(choice)))
          values.push_back(std::string(choice));
      if (choices.empty() && allowed != nullptr)
        for (const auto& choice : *allowed)
          values.push_back(choice);
      return {{"type", "string"}, {"enum", std::move(values)}};
    }

    // `allowedFirst`, when set, limits the first positional (or the subcommand name) to those choices.
    json callSchema(const helix::cli::Command& command, const std::set<std::string>* allowedFirst) {
      json prefix = json::array();
      std::size_t required = 0;
      bool open = false; // accepts any number of further strings

      if (!command.subcommands.empty()) {
        // `color-scheme-set builtin <name>`: the subcommand name, then its own positionals as plain strings.
        std::set<std::string> names;
        for (const auto& sub : command.subcommands)
          names.insert(std::string(sub.name));
        prefix.push_back(stringSchema({}, allowedFirst ? allowedFirst : &names));
        required = 1;
        open = true;
      } else {
        const auto offered = offeredPositionals(command);
        for (std::size_t i = 0; i < offered.size(); ++i) {
          const auto& positional = *offered[i];
          prefix.push_back(stringSchema(positional.choices, i == 0 ? allowedFirst : nullptr));
          if (positional.required)
            required = i + 1;
          if (positional.variadic || positional.joinRemaining)
            open = true;
        }
      }

      json args = {{"type", "array"}, {"minItems", required}};
      if (!prefix.empty())
        args["prefixItems"] = prefix;
      if (open)
        args["items"] = stringSchema({});
      else
        args["items"] = false; // nothing beyond the declared positionals
      if (!open)
        args["maxItems"] = prefix.size();

      return {
          {"type", "object"},
          {"properties",
           {{"tool", {{"const", std::string(command.name)}}},
            {"args", std::move(args)},
            {"reason", {{"type", "string"}, {"maxLength", 200}}}}},
          {"required", json::array({"tool", "args"})},
          {"additionalProperties", false},
      };
    }

  } // namespace

  json toolCallSchema(
      const helix::cli::Command& msgRoot, const RiskPolicy& policy, const std::set<Tier>& tiers,
      const std::set<std::string>* onlyTools, bool includeDecline
  ) {
    json alternatives = json::array();
    for (const auto& command : msgRoot.subcommands) {
      const std::string name(command.name);
      if (!policy.knows(name))
        continue; // an unclassified command is blocked, so it is never offered
      if (onlyTools && !onlyTools->contains(name))
        continue;
      if (tiers.contains(policy.tierFor(name))) {
        alternatives.push_back(callSchema(command, nullptr));
        continue;
      }
      std::set<std::string> granted;
      for (const auto& [choice, tier] : policy.choiceTiers(name))
        if (tiers.contains(tier))
          granted.insert(choice);
      if (!granted.empty())
        alternatives.push_back(callSchema(command, &granted));
    }
    if (includeDecline) {
      alternatives.push_back({
          {"type", "object"},
          {"properties",
           {{"tool", {{"const", kDeclineTool}}},
            {"args", {{"type", "array"}, {"minItems", 0}, {"maxItems", 0}, {"items", false}}},
            {"reason", {{"type", "string"}, {"minLength", 1}, {"maxLength", 200}}}}},
          {"required", json::array({"tool", "args", "reason"})},
          {"additionalProperties", false},
      });
    }
    return {
        {"$schema", "https://json-schema.org/draft/2020-12/schema"},
        {"title", "Helix desktop tool call"},
        {"oneOf", std::move(alternatives)},
    };
  }

  std::expected<ToolHints, std::string> ToolHints::parse(std::string_view text) {
    toml::table table;
    try {
      table = toml::parse(text);
    } catch (const toml::parse_error& error) {
      return std::unexpected(std::string("tool hints: ") + std::string(error.description()));
    }
    ToolHints out;
    const auto* section = table["hints"].as_table();
    if (!section)
      return std::unexpected("tool hints: missing [hints]");
    for (const auto& [key, node] : *section) {
      const auto value = node.value<std::string>();
      if (!value || value->empty())
        return std::unexpected("tool hints: '" + std::string(key.str()) + "' must be a non-empty string");
      out.hints_.emplace(std::string(key.str()), *value);
    }
    return out;
  }

  std::expected<ToolHints, std::string> ToolHints::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in)
      return std::unexpected("tool hints: cannot read " + file.string());
    std::stringstream text;
    text << in.rdbuf();
    return parse(text.str());
  }

  const std::string* ToolHints::find(std::string_view command) const {
    const auto it = hints_.find(command);
    return it == hints_.end() ? nullptr : &it->second;
  }

  std::vector<std::string> ToolHints::commands() const {
    std::vector<std::string> out;
    for (const auto& entry : hints_)
      out.push_back(entry.first);
    return out;
  }

  std::string toolCatalog(
      const helix::cli::Command& msgRoot, const RiskPolicy& policy, const std::set<Tier>& tiers,
      const std::set<std::string>* onlyTools, bool includeDecline, const ToolHints* hints
  ) {
    std::string out;
    for (const auto& command : msgRoot.subcommands) {
      const std::string name(command.name);
      if (!policy.knows(name))
        continue;
      if (onlyTools && !onlyTools->contains(name))
        continue;
      std::set<std::string> granted; // choices of a command that is blocked as a whole
      const bool whole = tiers.contains(policy.tierFor(name));
      if (!whole) {
        for (const auto& [choice, tier] : policy.choiceTiers(name))
          if (tiers.contains(tier))
            granted.insert(choice);
        if (granted.empty())
          continue;
      }
      out += "- " + name;
      const auto offered = offeredPositionals(command);
      for (std::size_t i = 0; i < offered.size(); ++i) {
        const auto& positional = *offered[i];
        std::string label = std::string(positional.name);
        if (i == 0 && !whole) {
          label += ": ";
          for (const auto& choice : granted)
            label += (label.back() == ' ' ? "" : "|") + choice;
        } else if (!positional.choices.empty()) {
          label += ": ";
          for (const std::string_view choice : positional.choices)
            label += (label.back() == ' ' ? "" : "|") + std::string(choice);
        }
        out += positional.required ? " <" + label + ">" : " [" + label + "]";
      }
      if (!command.subcommands.empty()) {
        out += " <";
        for (const auto& sub : command.subcommands)
          out += (out.back() == '<' ? "" : "|") + std::string(sub.name);
        out += "> ...";
      }
      const std::string* hint = hints ? hints->find(name) : nullptr;
      if (hint)
        out += " - " + *hint;
      else if (!command.summary.empty())
        out += " - " + std::string(command.summary);
      out += "\n";
    }
    if (includeDecline)
      out += std::string("- ")
          + kDeclineTool
          + " - Use this when none of the other tools can do what was asked, or it would be unsafe. Put the reason in "
            "\"reason\".\n";
    return out;
  }

} // namespace helix::agent
