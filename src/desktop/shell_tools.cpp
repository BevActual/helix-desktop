#include "desktop/shell_tools.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace helix::agent {

  std::filesystem::path shellRuntimeDir() {
    const char* override = std::getenv("HELIX_AGENT_IPC_DIR");
    if (override && *override)
      return override;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime && *runtime ? std::filesystem::path(runtime) : std::filesystem::path();
  }

  std::set<std::string> shellSupportedTools() {
    namespace fs = std::filesystem;
    std::set<std::string> names;
    const fs::path runtime = shellRuntimeDir();
    std::error_code ec;
    if (runtime.empty())
      return names;
    fs::path newest;
    fs::file_time_type newestTime{};
    for (const auto& entry : fs::directory_iterator(runtime, ec)) {
      if (!entry.path().filename().string().starts_with("helix-shell-tools-"))
        continue;
      const auto time = entry.last_write_time(ec);
      std::ifstream in(entry.path());
      std::string first;
      if (!std::getline(in, first) || first.empty() || !std::all_of(first.begin(), first.end(), [](char c) {
            return c >= '0' && c <= '9';
          }))
        continue;
      if (!fs::exists(fs::path("/proc") / first, ec))
        continue; // the shell that wrote it has gone
      if (newest.empty() || time > newestTime) {
        newest = entry.path();
        newestTime = time;
      }
    }
    if (newest.empty())
      return names;
    std::ifstream in(newest);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line))
      if (!line.empty() && line.size() <= 80)
        names.insert(line);
    return names;
  }

} // namespace helix::agent
