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
// Free-text arguments must be non-empty: an empty string is never a meaningful value for a command.
// Strings are bounded in the grammar (free-text arguments 1 to 120 characters, a reason up to 200) so that a model that
// gets stuck inside one cannot run on until the token limit; the service validates the finished reply against the schema
// regardless.
struct GrammarOptions {
  // Emit {"reason": ..., "tool": ..., "args": ...} instead of {"tool": ..., "args": ..., "reason": ...}. A reason written
  // before the tool can act as a short chain of thought; written after, it can only explain a choice already made. The
  // reason is then required and kept short (1 to 160 characters).
  bool reasonFirst = false;
};

std::string grammarFromToolSchema(const nlohmann::json& schema, const GrammarOptions& options = {});

}  // namespace helix::inference
