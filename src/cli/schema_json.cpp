#include "cli/schema_json.h"

#include "cli/schema_msg.h"

#include <iostream>
#include <nlohmann/json.hpp>

namespace helix::cli {

  namespace {

    nlohmann::json choicesJson(std::span<const std::string_view> choices) {
      auto out = nlohmann::json::array();
      for (const std::string_view choice : choices)
        out.push_back(std::string(choice));
      return out;
    }

    nlohmann::json commandJson(const Command& command) {
      nlohmann::json out = {
          {"name", std::string(command.name)},
          {"summary", std::string(command.summary)},
          {"hidden", command.hidden},
      };

      auto positionals = nlohmann::json::array();
      for (const Positional& positional : command.positionals) {
        positionals.push_back({
            {"name", std::string(positional.name)},
            {"description", std::string(positional.description)},
            {"required", positional.required},
            {"variadic", positional.variadic},
            {"joinRemaining", positional.joinRemaining},
            {"choices", choicesJson(positional.choices)},
        });
      }
      out["positionals"] = std::move(positionals);

      auto flags = nlohmann::json::array();
      for (const Flag& flag : command.flags) {
        flags.push_back({
            {"name", std::string(canonicalName(flag))},
            {"valueName", std::string(flag.valueName)},
            {"required", flag.required},
            {"repeatable", flag.repeatable},
            {"choices", choicesJson(flag.choices)},
        });
      }
      out["flags"] = std::move(flags);

      auto subcommands = nlohmann::json::array();
      for (const Command& child : command.subcommands)
        subcommands.push_back(commandJson(child));
      out["subcommands"] = std::move(subcommands);
      return out;
    }

  } // namespace

  std::string generateMsgSchemaJson(const Command& msgRoot) {
    auto commands = nlohmann::json::array();
    for (const Command& command : msgRoot.subcommands)
      commands.push_back(commandJson(command));
    return nlohmann::json{{"version", 1}, {"commands", std::move(commands)}}.dump(2);
  }

  int runSchemaJsonCli(int /*argc*/, char* /*argv*/[]) {
    std::cout << generateMsgSchemaJson(kMsgCmd) << '\n';
    return 0;
  }

} // namespace helix::cli
