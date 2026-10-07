#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace helix::inference {

// Converts the tool-call schema from desktop/tool_schema.h into a GBNF grammar (the engine's constrained-output format),
// so a model can only produce text that conforms to it. Only the subset that schema uses is supported:
//   {"oneOf": [ {type: object, properties: {tool: {const}, args: {prefixItems, minItems, items}, reason}, ...} ]}
// where array items are plain strings or string enums. Anything else is an error rather than a silently wrong grammar.
//
// The output is compact JSON in a fixed key order: {"tool":"<name>","args":[...]} with an optional ,"reason":"<text>".
// String length limits in the schema are not expressed in the grammar (the generation token limit bounds them); the
// service validates the finished reply against the schema regardless.
std::string grammarFromToolSchema(const nlohmann::json& schema);

}  // namespace helix::inference
