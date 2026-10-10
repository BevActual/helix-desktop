#pragma once

#include "cli/schema.h"

#include <string>

namespace helix::cli {

  [[nodiscard]] std::string generateBash(const Command& root);
  [[nodiscard]] std::string generateZsh(const Command& root);
  [[nodiscard]] std::string generateFish(const Command& root);

  int runCompletionsCli(int argc, char* argv[]);

} // namespace helix::cli
