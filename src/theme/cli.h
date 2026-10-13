#pragma once

namespace helix::theme {

  // Entry point for `helix theme <image> [options]`. Returns a process exit
  // code. Does not touch Application / event loop / config — pure function of
  // (argv, stdout, stderr).
  int runCli(int argc, char* argv[]);

} // namespace helix::theme
