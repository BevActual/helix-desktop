#pragma once

namespace helix::config {

  // Entry point for `helix config <command> [options]`. Returns a process
  // exit code. Pure CLI helper; does not start Application or mutate live config.
  int runCli(int argc, char* argv[]);

} // namespace helix::config
