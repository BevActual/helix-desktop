#include "ipc/cli.h"

#include "cli/help.h"
#include "cli/schema_msg.h"
#include "ipc/ipc_client.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <string_view>

namespace helix::ipc {

  int runCli(int argc, char* argv[]) {
    if (argc < 3) {
      std::fputs("error: msg requires a command (try: helix-panel --help)\n", stderr);
      return 1;
    }

    const std::string_view command = argv[2];
    if (command == "--help" || command == "-h") {
      std::print("{}", cli::renderHelp(cli::kMsgCmd, "helix-panel"));
      return 0;
    }
    // A `--` before the final argument ends option parsing, so a trailing "--help" is payload.
    bool endOfOptions = false;
    for (int i = 3; i < argc - 1; ++i)
      endOfOptions = endOfOptions || std::string_view(argv[i]) == "--";
    if (!endOfOptions && argc >= 4
        && (std::string_view(argv[argc - 1]) == "--help" || std::string_view(argv[argc - 1]) == "-h")) {
      if (const cli::Command* spec = cli::findMsgCommand(command); spec != nullptr) {
        std::string path = "helix-panel ";
        path.append(command);
        std::print("{}", cli::renderHelp(*spec, path));
        return 0;
      }
    }

    if (std::string_view(argv[2]) == "notification-show" && argc >= 5) {
      nlohmann::json payload = {
          {"summary", std::string(argv[3])},
          {"body", std::string(argv[4])},
      };
      if (argc > 5) {
        std::string body(argv[4]);
        for (int i = 5; i < argc; ++i) {
          body += ' ';
          body += argv[i];
        }
        payload["body"] = std::move(body);
      }
      return IpcClient::send(std::string(argv[2]) + " " + payload.dump());
    }

    std::string cmd = argv[2];
    bool consumedEndOfOptions = false;
    for (int i = 3; i < argc; ++i) {
      // The first `--` is a client-side marker; the server only receives the payload after it.
      if (!consumedEndOfOptions && std::string_view(argv[i]) == "--") {
        consumedEndOfOptions = true;
        continue;
      }
      cmd += ' ';
      cmd += argv[i];
    }
    return IpcClient::send(cmd);
  }

} // namespace helix::ipc
