#include "cli/schema_msg.h"
#include "desktop/tool_schema.h"
#include "inference/schema_grammar.h"
#include "tests/test_check.h"

#include <filesystem>
#include <map>
#include <set>
#include <sstream>

using namespace helix;
using nlohmann::json;

namespace {

bool throws(const json& schema) {
  try { (void)inference::grammarFromToolSchema(schema); } catch (const std::exception&) { return true; }
  return false;
}

// Splits "name ::= rhs" lines into a map.
std::map<std::string, std::string> parseRules(const std::string& grammar) {
  std::map<std::string, std::string> rules;
  std::istringstream in(grammar);
  for (std::string line; std::getline(in, line);) {
    const auto split = line.find(" ::= ");
    if (split == std::string::npos) continue;
    rules[line.substr(0, split)] = line.substr(split + 5);
  }
  return rules;
}

// Rule names mentioned in a right-hand side, ignoring quoted literals and [character classes].
std::set<std::string> referenced(const std::string& rhs) {
  std::set<std::string> names;
  for (std::size_t i = 0; i < rhs.size();) {
    if (rhs[i] == '"') {  // skip a literal
      for (++i; i < rhs.size() && rhs[i] != '"'; ++i) if (rhs[i] == '\\') ++i;
      ++i;
    } else if (rhs[i] == '[') {  // skip a character class
      for (++i; i < rhs.size() && rhs[i] != ']'; ++i) if (rhs[i] == '\\') ++i;
      ++i;
    } else if ((rhs[i] >= 'a' && rhs[i] <= 'z')) {
      std::size_t end = i;
      while (end < rhs.size() && ((rhs[end] >= 'a' && rhs[end] <= 'z') || (rhs[end] >= '0' && rhs[end] <= '9') || rhs[end] == '-')) ++end;
      names.insert(rhs.substr(i, end - i));
      i = end;
    } else {
      ++i;
    }
  }
  return names;
}

json tool(const std::string& name, const json& args) {
  return {{"type", "object"}, {"properties", {{"tool", {{"const", name}}}, {"args", args}}}};
}

}  // namespace

int main(int argc, char** argv) {
  TEST_CHECK(argc >= 2);
  auto policy = agent::RiskPolicy::load(std::filesystem::path(argv[1]) / "protocols/ipc/msg-risk.toml");
  TEST_CHECK(policy.has_value());
  const auto schema = agent::toolCallSchema(noctalia::cli::kMsgCmd, *policy,
                                            {agent::Tier::Observe, agent::Tier::ObservePrivate, agent::Tier::Act});
  const std::string grammar = inference::grammarFromToolSchema(schema);
  const auto rules = parseRules(grammar);

  // Structure: a root, and no rule is used without being defined.
  TEST_CHECK(rules.contains("root") && rules.contains("call") && rules.contains("str") && rules.contains("ws"));
  for (const auto& [name, rhs] : rules)
    for (const auto& used : referenced(rhs))
      if (!rules.contains(used)) { std::fprintf(stderr, "rule %s uses undefined %s\n", name.c_str(), used.c_str()); TEST_CHECK(false); }

  // One rule per offered tool, named after it, and the grammar offers nothing the schema does not.
  for (const auto& alternative : schema.at("oneOf")) {
    const auto tool = alternative.at("properties").at("tool").at("const").get<std::string>();
    TEST_CHECK(rules.contains("call-" + tool));
  }
  std::size_t callRules = 0;
  for (const auto& [name, rhs] : rules) callRules += name.starts_with("call-") ? 1 : 0;
  TEST_CHECK(callRules == schema.at("oneOf").size());

  // Argument shapes.
  TEST_CHECK(rules.at("call-status").find("\"[\" ws ws \"]\"") != std::string::npos);                 // no arguments
  TEST_CHECK(rules.at("call-media").find("\"toggle\"") != std::string::npos);                          // an enum
  TEST_CHECK(rules.at("call-media").find("\"explode\"") == std::string::npos);
  auto alternativesOf = [&](const char* tool) {  // how many argument counts the rule accepts
    std::size_t count = 0;
    for (std::size_t at = 0; (at = rules.at(tool).find("\"[\" ws", at)) != std::string::npos; ++at) ++count;
    return count;
  };
  TEST_CHECK(alternativesOf("call-status") == 1);          // exactly zero arguments
  TEST_CHECK(alternativesOf("call-panel-open") == 2);      // one or two, never none
  TEST_CHECK(alternativesOf("call-volume-set") == 1);      // exactly one
  TEST_CHECK(alternativesOf("call-notification-show") == 1);  // two required, then any number of further words
  TEST_CHECK(rules.at("call-notification-show").find("(\",\" ws str-nonempty)*") != std::string::npos);  // trailing words
  // `session` is blocked as a whole; only the granted choice may appear.
  TEST_CHECK(rules.at("call-session").find("\"lock\"") != std::string::npos);
  TEST_CHECK(rules.at("call-session").find("reboot") == std::string::npos && rules.at("call-session").find("shutdown") == std::string::npos);
  // Nothing that needs approval or is blocked is in the grammar at all.
  for (const char* absent : {"call-app-launch", "call-window-close", "call-firewall-panic-toggle", "call-clipboard-copy"})
    TEST_CHECK(!rules.contains(absent));

  // `decline` carries a mandatory reason; ordinary tools keep it optional.
  {
    const auto declining = inference::grammarFromToolSchema(agent::toolCallSchema(noctalia::cli::kMsgCmd, *policy, {agent::Tier::Observe}, nullptr, true));
    const auto declineRules = parseRules(declining);
    TEST_CHECK(declineRules.contains("call-decline"));
    TEST_CHECK(declineRules.at("call-decline").find("reason") != std::string::npos && declineRules.at("call-decline").find(" reason ") == std::string::npos);
    TEST_CHECK(declineRules.at("call-decline").find("str-nonempty") != std::string::npos);   // a reason cannot be empty
    TEST_CHECK(declineRules.at("call-status").find("str-nonempty") == std::string::npos);
    TEST_CHECK(declineRules.at("str-nonempty").find("char+") != std::string::npos);
    TEST_CHECK(declineRules.at("call-status").find(" reason ") != std::string::npos);  // the optional rule
    for (const auto& [name, rhs] : declineRules)
      for (const auto& used : referenced(rhs)) TEST_CHECK(declineRules.contains(used));
  }

  // Free text is never empty: every free-text position uses the non-empty rule, none the empty-allowing one.
  for (const auto& [name, rhs] : rules)
    if (name.starts_with("call-")) {
      TEST_CHECK(rhs.find(" str ") == std::string::npos && rhs.find("(str ") == std::string::npos);
    }
  TEST_CHECK(rules.at("call-volume-set").find("str-nonempty") != std::string::npos);

  // Reason first: every call starts with a short mandatory reason, written before the tool name.
  {
    inference::GrammarOptions first;
    first.reasonFirst = true;
    const auto reasoned = parseRules(inference::grammarFromToolSchema(schema, first));
    TEST_CHECK(reasoned.at("reason-short").find("char{1,160}") != std::string::npos);
    std::size_t calls = 0;
    for (const auto& [name, rhs] : reasoned) {
      for (const auto& used : referenced(rhs)) TEST_CHECK(reasoned.contains(used));
      if (!name.starts_with("call-")) continue;
      ++calls;
      const auto reasonAt = rhs.find("reason"), toolAt = rhs.find("tool");
      TEST_CHECK(reasonAt != std::string::npos && toolAt != std::string::npos && reasonAt < toolAt);
      TEST_CHECK(rhs.find("reason-short") != std::string::npos);
    }
    TEST_CHECK(calls == callRules);  // exactly the same tools as the default ordering
  }

  // Quoting: a value with a quote or backslash must stay inside its literal.
  const json tricky = {{"oneOf", json::array({tool("pick", {{"prefixItems", json::array({{{"type", "string"}, {"enum", json::array({"a\"b", "c\\d"})}}})}, {"minItems", 1}, {"items", false}})})}};
  const auto quoted = inference::grammarFromToolSchema(tricky);
  TEST_CHECK(quoted.find("\"a\\\"b\"") != std::string::npos && quoted.find("\"c\\\\d\"") != std::string::npos);

  // Unsupported or unsafe schemas fail loudly instead of producing a wrong grammar.
  TEST_CHECK(throws(json::object()));                                                                  // no oneOf
  TEST_CHECK(throws({{"oneOf", json::array()}}));                                                      // nothing to choose
  TEST_CHECK(throws({{"oneOf", json::array({tool("Bad Name", {{"minItems", 0}})})}}));                 // uppercase and a space
  TEST_CHECK(throws({{"oneOf", json::array({tool("a", {{"prefixItems", json::array({{{"type", "number"}}})}, {"minItems", 1}, {"items", false}})})}}));
  TEST_CHECK(throws({{"oneOf", json::array({tool("a", {{"minItems", 0}}), tool("a", {{"minItems", 0}})})}}));  // duplicate
  TEST_CHECK(throws({{"oneOf", json::array({tool("a", {{"prefixItems", json::array({{{"type", "string"}, {"enum", json::array()}}})}, {"minItems", 1}, {"items", false}})})}}));
  return 0;
}
