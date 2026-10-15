// Schema-normalized effective config for Rust `helix-desktop` template apply (`config_export::serialize`).
#include "config/config_service.h"
#include "util/file_utils.h"

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: helix-config-export\n"
                   "Print schema-normalized effective config (config_export::serialize) on stdout.\n";
      return 0;
    }
    std::cerr << "error: unknown argument: " << arg << '\n';
    return 1;
  }

  std::string error;
  const std::string config_dir = FileUtils::configDir();
  std::string settings_path;
  if (const std::string state = FileUtils::stateDir(); !state.empty()) {
    settings_path = state + "/settings.toml";
  }
  const std::string toml = ConfigService::buildEffectiveConfigFromSources(config_dir, settings_path, &error);
  if (toml.empty()) {
    std::cerr << (error.empty() ? "config export failed" : error) << '\n';
    return 1;
  }
  std::cout << toml;
  return 0;
}
