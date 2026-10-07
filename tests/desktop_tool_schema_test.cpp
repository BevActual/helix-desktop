#include "cli/schema_msg.h"
#include "desktop/tool_schema.h"
#include "tests/test_check.h"

#include <algorithm>
#include <filesystem>
#include <string>

using namespace helix::agent;
using nlohmann::json;

namespace {

// A validator for exactly the JSON Schema subset the generator emits.
bool valid(const json& schema, const json& value) {
  if (schema.is_boolean()) return schema.get<bool>();
  if (schema.contains("oneOf")) {
    int matches = 0;
    for (const auto& alternative : schema.at("oneOf")) matches += valid(alternative, value) ? 1 : 0;
    if (matches != 1) return false;
  }
  if (schema.contains("const") && value != schema.at("const")) return false;
  if (schema.contains("enum") && std::ranges::find(schema.at("enum"), value) == schema.at("enum").end()) return false;
  if (schema.contains("type")) {
    const auto type = schema.at("type").get<std::string>();
    if ((type == "string" && !value.is_string()) || (type == "array" && !value.is_array()) ||
        (type == "object" && !value.is_object())) return false;
  }
  if (value.is_string() && schema.contains("maxLength") && value.get<std::string>().size() > schema.at("maxLength").get<std::size_t>())
    return false;
  if (value.is_object()) {
    const auto properties = schema.value("properties", json::object());
    for (const auto& [key, child] : value.items()) {
      if (properties.contains(key)) { if (!valid(properties.at(key), child)) return false; }
      else if (schema.value("additionalProperties", true) == false) return false;
    }
    for (const auto& key : schema.value("required", json::array())) if (!value.contains(key.get<std::string>())) return false;
  }
  if (value.is_array()) {
    if (schema.contains("minItems") && value.size() < schema.at("minItems").get<std::size_t>()) return false;
    if (schema.contains("maxItems") && value.size() > schema.at("maxItems").get<std::size_t>()) return false;
    const auto prefix = schema.value("prefixItems", json::array());
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (i < prefix.size()) { if (!valid(prefix[i], value[i])) return false; }
      else if (schema.contains("items") && !valid(schema.at("items"), value[i])) return false;
    }
  }
  return true;
}

json call(const std::string& tool, json args = json::array()) { return {{"tool", tool}, {"args", std::move(args)}}; }

bool offers(const json& schema, const std::string& tool) {
  return std::ranges::any_of(schema.at("oneOf"), [&](const json& alternative) {
    return alternative.at("properties").at("tool").at("const") == tool;
  });
}

}  // namespace

int main(int argc, char** argv) {
  TEST_CHECK(argc >= 2);
  auto policy = RiskPolicy::load(std::filesystem::path(argv[1]) / "protocols/ipc/msg-risk.toml");
  TEST_CHECK(policy.has_value());
  const auto& root = noctalia::cli::kMsgCmd;

  // Read-only and everyday tiers: no command that needs approval, and nothing blocked.
  const auto everyday = toolCallSchema(root, *policy, {Tier::Observe, Tier::ObservePrivate, Tier::Act});
  TEST_CHECK(everyday.at("$schema").get<std::string>().find("2020-12") != std::string::npos);
  for (const char* offered : {"status", "panel-open", "volume-set", "media", "clipboard-text", "notification-show"})
    TEST_CHECK(offers(everyday, offered));
  for (const char* withheld : {"app-launch", "window-close", "clipboard-copy", "session", "firewall-panic-toggle", "wifi-toggle"})
    if (std::string(withheld) != "session") TEST_CHECK(!offers(everyday, withheld));
  // `session` is blocked as a whole but `session lock` is an `act` override, so it appears limited to that choice.
  TEST_CHECK(offers(everyday, "session"));
  TEST_CHECK(valid(everyday, call("session", {"lock"})));
  TEST_CHECK(!valid(everyday, call("session", {"reboot"})));
  TEST_CHECK(!valid(everyday, call("session", {"suspend"})));  // a confirm-tier choice is not offered here

  // Every unblocked command in these tiers is offered exactly once.
  std::size_t expected = 0;
  for (const auto& command : root.subcommands) {
    const auto tier = policy->tierFor(std::string(command.name));
    expected += (tier == Tier::Observe || tier == Tier::ObservePrivate || tier == Tier::Act) ? 1 : 0;
  }
  const std::size_t withLockOverride = expected + 1;  // `session`
  TEST_CHECK(everyday.at("oneOf").size() == withLockOverride);

  // Valid calls are accepted.
  TEST_CHECK(valid(everyday, call("status")));
  TEST_CHECK(valid(everyday, call("panel-open", {"control-center"})));
  TEST_CHECK(valid(everyday, call("panel-open", {"settings", "appearance"})));
  TEST_CHECK(valid(everyday, call("volume-set", {"50"})));
  TEST_CHECK(valid(everyday, call("media", {"toggle"})));
  TEST_CHECK(valid(everyday, call("notification-show", {"Title", "a body", "with more words"})));  // joined trailing text
  TEST_CHECK(valid(everyday, json{{"tool", "status"}, {"args", json::array()}, {"reason", "checking"}}));

  // Malformed calls are not.
  TEST_CHECK(!valid(everyday, call("no-such-command")));
  TEST_CHECK(!valid(everyday, call("status", {"extra"})));               // too many arguments
  TEST_CHECK(!valid(everyday, call("panel-open")));                      // missing required argument
  TEST_CHECK(!valid(everyday, call("panel-open", {"a", "b", "c"})));     // more than declared
  TEST_CHECK(!valid(everyday, call("media", {"explode"})));              // not one of the enum values
  TEST_CHECK(!valid(everyday, call("app-launch", {"Settings"})));        // needs a tier this schema does not grant
  TEST_CHECK(!valid(everyday, call("window-close")));
  TEST_CHECK(!valid(everyday, json{{"tool", "status"}}));                // args is required
  TEST_CHECK(!valid(everyday, json{{"tool", "status"}, {"args", json::array()}, {"sudo", true}}));  // no extra fields
  TEST_CHECK(!valid(everyday, call("panel-open", {std::string(600, 'x')})));  // free text is bounded

  // With the confirm tier, the commands that ask the user are offered too, and blocked ones still are not.
  const auto withConfirm = toolCallSchema(root, *policy, {Tier::Observe, Tier::ObservePrivate, Tier::Act, Tier::Confirm});
  TEST_CHECK(valid(withConfirm, call("app-launch", {"Settings"})));
  TEST_CHECK(valid(withConfirm, call("window-close")));
  TEST_CHECK(valid(withConfirm, call("session", {"suspend"})));
  TEST_CHECK(!valid(withConfirm, call("session", {"reboot"})));          // blocked in every tier set
  TEST_CHECK(!valid(withConfirm, call("session", {"shutdown"})));
  TEST_CHECK(!offers(withConfirm, "firewall-panic-toggle"));

  // Observe only: a much smaller menu, and nothing that changes anything.
  const auto observeOnly = toolCallSchema(root, *policy, {Tier::Observe});
  TEST_CHECK(offers(observeOnly, "status") && !offers(observeOnly, "panel-open") && !offers(observeOnly, "session"));
  TEST_CHECK(!valid(observeOnly, call("volume-set", {"50"})));
  TEST_CHECK(toolCallSchema(root, *policy, {}).at("oneOf").empty());

  // Subcommand grammar: the subcommand name is an enum, then its own text arguments.
  TEST_CHECK(valid(everyday, call("color-scheme-set", {"builtin", "Nord"})));
  TEST_CHECK(!valid(everyday, call("color-scheme-set", {"bogus", "Nord"})));
  return 0;
}
