#pragma once

#include "cli/schema.h"

#include <string>

namespace noctalia::cli {

  // Machine-readable dump of the `msg` command schema, including hidden commands.
  // Consumed by tools that generate typed wrappers (the agent MCP server) and by the
  // risk-policy coverage test.
  [[nodiscard]] std::string generateMsgSchemaJson(const Command& msgRoot);

  int runSchemaJsonCli(int argc, char* argv[]);

} // namespace noctalia::cli
