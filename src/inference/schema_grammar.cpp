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
  if (item.value("type", "") == "string") return "str-nonempty";
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
    if (open && count == prefix.size()) body += count == 0 ? " (str-nonempty (\",\" ws str-nonempty)*)?" : " (\",\" ws str-nonempty)*";
    body += " ws \"]\"";
    alternatives.push_back(body);
  }
  std::string rule;
  for (const auto& alternative : alternatives) rule += (rule.empty() ? "" : " | ") + alternative;
  return rule;
}

}  // namespace

std::string grammarFromToolSchema(const json& schema, const GrammarOptions& options) {
  if (!schema.contains("oneOf") || !schema.at("oneOf").is_array()) throw std::invalid_argument("expected a oneOf of tool calls");

  std::string calls, rules;
  std::set<std::string> seen;
  for (const auto& alternative : schema.at("oneOf")) {
    const auto& properties = alternative.at("properties");
    const std::string tool = properties.at("tool").at("const").get<std::string>();
    const std::string name = ruleName(tool);
    if (!seen.insert(name).second) throw std::invalid_argument("duplicate tool: " + tool);
    const std::string head = "\"{\" ws ";
    const std::string toolKey = "\"\\\"tool\\\"\" ws \":\" ws " + literal("\"" + tool + "\"");
    const std::string argsKey = "\"\\\"args\\\"\" ws \":\" ws (" + argsRule(properties.at("args")) + ")";
    const std::string reasonKey = "\"\\\"reason\\\"\" ws \":\" ws ";
    if (options.reasonFirst) {
      // The reason comes first and is always present, so it is written before the model commits to a tool.
      rules += name + " ::= " + head + reasonKey + "reason-short ws \",\" ws " + toolKey + " ws \",\" ws " + argsKey + " ws \"}\"\n";
    } else {
      // A tool whose schema requires `reason` must carry it; otherwise it is optional.
      bool reasonRequired = false;
      for (const auto& key : alternative.value("required", json::array())) reasonRequired = reasonRequired || key == "reason";
      const std::string reason = reasonRequired ? "ws \",\" ws " + reasonKey + "str-nonempty" : "reason";
      rules += name + " ::= " + head + toolKey + " ws \",\" ws " + argsKey + " " + reason + " ws \"}\"\n";
    }
    calls += (calls.empty() ? "" : " | ") + name;
  }
  if (calls.empty()) throw std::invalid_argument("no tools to choose from");

  return "root ::= call\n"
         "call ::= " + calls + "\n" + rules +
         "reason ::= (ws \",\" ws \"\\\"reason\\\"\" ws \":\" ws str)?\n"
         "str ::= \"\\\"\" char{0,200} \"\\\"\"\n"
         "str-nonempty ::= \"\\\"\" char{1,120} \"\\\"\"\n"
         "reason-short ::= \"\\\"\" char{1,160} \"\\\"\"\n"
         "char ::= [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])\n"
         "ws ::= [ ]?\n";
}

}  // namespace helix::inference
