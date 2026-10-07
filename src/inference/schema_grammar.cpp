#include "inference/schema_grammar.h"

#include <set>
#include <stdexcept>
#include <vector>

namespace helix::inference {

namespace {

using nlohmann::json;

// A GBNF double-quoted literal containing exactly `text`.
std::string literal(const std::string& text) {
  std::string out = "\"";
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') out += '\\';
    out += ch;
  }
  return out + "\"";
}

// One array element: a JSON string, either any string or one of a fixed set.
std::string itemRule(const json& item) {
  if (item.contains("enum")) {
    std::string alternatives;
    for (const auto& value : item.at("enum")) {
      if (!value.is_string()) throw std::invalid_argument("only string enums are supported");
      alternatives += (alternatives.empty() ? "" : " | ") + literal(value.get<std::string>());
    }
    if (alternatives.empty()) throw std::invalid_argument("an enum with no values can never be satisfied");
    return "\"\\\"\" (" + alternatives + ") \"\\\"\"";
  }
  if (item.value("type", "") == "string") return "str";
  throw std::invalid_argument("array items must be strings or string enums");
}

std::string ruleName(const std::string& tool) {
  std::string name = "call-";
  for (const char ch : tool) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    if (!ok) throw std::invalid_argument("tool names must be lowercase letters, digits and hyphens");
    name += ch;
  }
  return name;
}

// `args` for one tool: every allowed element count from minItems up to the number of declared positions, and, when the
// schema allows further items, any number of extra strings after the declared ones.
std::string argsRule(const json& args) {
  const auto prefix = args.value("prefixItems", json::array());
  const std::size_t minItems = args.value("minItems", 0U);
  const bool open = args.contains("items") && !args.at("items").is_boolean();
  if (minItems > prefix.size() && !open) throw std::invalid_argument("minItems exceeds the declared positions");

  std::vector<std::string> alternatives;
  for (std::size_t count = minItems; count <= prefix.size(); ++count) {
    std::string body = "\"[\" ws";
    for (std::size_t i = 0; i < count; ++i) body += std::string(i == 0 ? " " : " \",\" ws ") + itemRule(prefix[i]);
    if (open && count == prefix.size()) body += count == 0 ? " (str (\",\" ws str)*)?" : " (\",\" ws str)*";
    body += " ws \"]\"";
    alternatives.push_back(body);
  }
  std::string rule;
  for (const auto& alternative : alternatives) rule += (rule.empty() ? "" : " | ") + alternative;
  return rule;
}

}  // namespace

std::string grammarFromToolSchema(const json& schema) {
  if (!schema.contains("oneOf") || !schema.at("oneOf").is_array()) throw std::invalid_argument("expected a oneOf of tool calls");

  std::string calls, rules;
  std::set<std::string> seen;
  for (const auto& alternative : schema.at("oneOf")) {
    const auto& properties = alternative.at("properties");
    const std::string tool = properties.at("tool").at("const").get<std::string>();
    const std::string name = ruleName(tool);
    if (!seen.insert(name).second) throw std::invalid_argument("duplicate tool: " + tool);
    calls += (calls.empty() ? "" : " | ") + name;
    rules += name + " ::= \"{\" ws \"\\\"tool\\\"\" ws \":\" ws " + literal("\"" + tool + "\"") + " ws \",\" ws \"\\\"args\\\"\" ws \":\" ws (" +
             argsRule(properties.at("args")) + ") reason ws \"}\"\n";
  }
  if (calls.empty()) throw std::invalid_argument("no tools to choose from");

  return "root ::= call\n"
         "call ::= " + calls + "\n" + rules +
         "reason ::= (ws \",\" ws \"\\\"reason\\\"\" ws \":\" ws str)?\n"
         "str ::= \"\\\"\" char* \"\\\"\"\n"
         "char ::= [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])\n"
         "ws ::= [ ]?\n";
}

}  // namespace helix::inference
