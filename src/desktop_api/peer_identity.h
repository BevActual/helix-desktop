#pragma once

#include <expected>
#include <gio/gio.h>
#include <string>
#include <vector>

namespace helix {

// Stable client identity from the verified executable and command line of a bus peer. Programs run by an
// interpreter would otherwise all share the interpreter's grants, so the script path is part of the identity.
std::string clientIdentity(const std::string& exePath, const std::vector<std::string>& cmdline);

struct PeerIdentity {
  std::string client;  // clientIdentity(exe, cmdline)
  std::string exe;
};

struct PeerFailure {
  bool wrongUser = false;  // the caller is another user's process (a policy refusal, not an error)
  std::string message;
};

// How much to learn about a caller. A service that runs in its own user namespace (any sandboxed user unit does) cannot
// read another process's /proc/<pid>/exe, so it can only identify callers by the process id and user the bus reports.
enum class PeerDetail {
  Executable,   // the verified executable and command line; fails if they cannot be read (use where grants depend on it)
  ProcessOnly,  // "pid:<n>" from the bus alone; enough for per-caller limits, not for deciding who a program is
};

// Who is on the other end of a bus connection, from the bus's own credentials for `sender` (never from arguments):
// the process id and user, then that process's executable and command line. Only same-user callers are accepted.
// Blocks briefly on a call to the bus daemon.
std::expected<PeerIdentity, PeerFailure> resolvePeer(GDBusConnection* bus, const std::string& sender,
                                                     PeerDetail detail = PeerDetail::Executable);

}  // namespace helix
