#pragma once

namespace helix::ipc {

  // Entry point for `helix-panel <command> [args...]`. Returns a process exit
  // code. Forwards the command to the running instance over the IPC socket.
  int runCli(int argc, char* argv[]);

} // namespace helix::ipc
