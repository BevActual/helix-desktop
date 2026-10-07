#include "cli/schema_msg.h"
#include "desktop/tool_schema.h"
#include "tests/schema_validator.h"
#include "tests/test_check.h"

#include <algorithm>
#include <filesystem>
#include <string>

using namespace helix::agent;
using helix::test::valid;
using nlohmann::json;

namespace {

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
  TEST_CHECK(!valid(everyday, call("panel-open", {""})));                       // and never empty

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

  // Optional free-text arguments are not offered, so nothing can be copied into them as a placeholder; enums and the
  // panel/settings `context` stay.
  TEST_CHECK(valid(everyday, call("bar-hide")) && !valid(everyday, call("bar-hide", {"top"})));
  TEST_CHECK(valid(everyday, call("wallpaper-next")) && !valid(everyday, call("wallpaper-next", {"eDP-1"})));
  TEST_CHECK(valid(everyday, call("brightness-set", {"60"})) && !valid(everyday, call("brightness-set", {"", "60"})));
  TEST_CHECK(!valid(everyday, call("brightness-set", {"eDP-1", "60"})) && !valid(everyday, call("brightness-set")));
  TEST_CHECK(valid(everyday, call("wallpaper-set", {"/home/me/a.png"})) && !valid(everyday, call("wallpaper-set", {"eDP-1", "/a.png"})));
  TEST_CHECK(valid(everyday, call("settings-open")) && valid(everyday, call("settings-open", {"network"})));
  TEST_CHECK(valid(everyday, call("panel-open", {"control-center", "firewall"})));
  TEST_CHECK(valid(everyday, call("screenshot-fullscreen", {"all"})) && valid(everyday, call("screenshot-fullscreen")));  // an enum stays optional

  // The decline option: only when asked for, reason required, and it takes no arguments.
  TEST_CHECK(!offers(everyday, "decline"));
  const auto withDecline = toolCallSchema(root, *policy, {Tier::Observe, Tier::ObservePrivate, Tier::Act}, nullptr, true);
  TEST_CHECK(offers(withDecline, "decline") && withDecline.at("oneOf").size() == everyday.at("oneOf").size() + 1);
  TEST_CHECK(valid(withDecline, json{{"tool", "decline"}, {"args", json::array()}, {"reason", "Rebooting is not something I can do."}}));
  TEST_CHECK(!valid(withDecline, call("decline")));                                                     // reason is required
  TEST_CHECK(!valid(withDecline, json{{"tool", "decline"}, {"args", json::array()}, {"reason", ""}})); // and must say something
  TEST_CHECK(!valid(withDecline, json{{"tool", "decline"}, {"args", json::array({"x"})}, {"reason", "no"}}));
  TEST_CHECK(valid(withDecline, call("status")));                                                       // real tools still work
  TEST_CHECK(toolCatalog(root, *policy, {Tier::Observe}, nullptr, true).find("- decline - Use this when none") != std::string::npos);
  TEST_CHECK(toolCatalog(root, *policy, {Tier::Observe}).find("decline") == std::string::npos);

  // The catalog lists exactly the tools the schema offers, with their arguments and a summary.
  const std::string catalog = toolCatalog(root, *policy, {Tier::Observe, Tier::ObservePrivate, Tier::Act});
  TEST_CHECK(catalog.find("- status") != std::string::npos && catalog.find("- media <action: next|previous|toggle") != std::string::npos);
  TEST_CHECK(catalog.find("- session <action: lock>") != std::string::npos && catalog.find("reboot") == std::string::npos);
  TEST_CHECK(catalog.find("- app-launch") == std::string::npos && catalog.find("- window-close") == std::string::npos);
  TEST_CHECK(catalog.find("- panel-open <id> [context]") != std::string::npos);
  TEST_CHECK(catalog.find("- brightness-set <value>") != std::string::npos && catalog.find("[target]") == std::string::npos);
  TEST_CHECK(catalog.find("- bar-hide -") != std::string::npos);

  // Hints replace the summary where they exist and change nothing else.
  const auto hints = ToolHints::parse("[hints]\nbrightness-up = \"Make the screen brighter (not sound)\"\n");
  TEST_CHECK(hints.has_value());
  const std::string hinted = toolCatalog(root, *policy, {Tier::Observe, Tier::ObservePrivate, Tier::Act}, nullptr, false, &*hints);
  TEST_CHECK(hinted.find("- brightness-up - Make the screen brighter (not sound)\n") != std::string::npos);
  TEST_CHECK(hinted.find("- bar-hide -") != std::string::npos);  // no hint: its own summary stays
  TEST_CHECK(!ToolHints::parse("[hints]\nx = \"\"\n").has_value() && !ToolHints::parse("nothing = 1").has_value());
  // A hint never adds a tool the policy does not offer.
  const auto sneaky = ToolHints::parse("[hints]\nsession = \"anything\"\nwindow-close = \"Close the window\"\n");
  TEST_CHECK(toolCatalog(root, *policy, {Tier::Observe}, nullptr, false, &*sneaky).find("window-close") == std::string::npos);
  std::size_t lines = 0;
  for (const char ch : catalog) lines += ch == '\n' ? 1 : 0;
  TEST_CHECK(lines == everyday.at("oneOf").size());
  return 0;
}
