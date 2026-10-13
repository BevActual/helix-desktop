#pragma once

#include <string>

class IpcClient {
public:
  // Sends `command` to the running helix instance.
  // Prints the response to stdout. Returns 0 on success, 1 on error.
  static int send(const std::string& command);

  // Sends one complete wire line (including any cwd prefix) and returns the response without printing.
  // `socketPath` overrides the default location. On failure returns false and sets `error` (no "error: " prefix).
  static bool
  exchange(const std::string& line, std::string& response, std::string& error, const std::string& socketPath = {});
};
