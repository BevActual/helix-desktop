#pragma once
// What the running shell says it can carry out. A shell that serves the agent command socket (the Tauri shell) writes
// `helix-shell-tools-<display>` (first line its pid, then command names); the service lists only those tools and the
// assistant offers the model only those, so the model is never asked to choose something that cannot work. With no live
// list (the native shell, or no shell) every classified tool counts.

#include <filesystem>
#include <set>
#include <string>

namespace helix::agent {

  // The runtime directory, or the folder HELIX_AGENT_IPC_DIR names (a test's).
  std::filesystem::path shellRuntimeDir();

  // The command names of the newest list written by a shell that is still running; empty when there is none.
  std::set<std::string> shellSupportedTools();

} // namespace helix::agent
