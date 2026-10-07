#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Policy core for org.helix.Desktop.Agent1: risk tiers, per-client grants, taint and audit.
// No D-Bus or shell dependencies, so it can be tested alone. Design: docs/design/agent-and-inference.md.
namespace helix::agent {

enum class Tier { Observe, ObservePrivate, Act, Confirm, Blocked };

std::string_view tierName(Tier tier);
std::optional<Tier> parseTier(std::string_view name);

// protocols/ipc/msg-risk.toml. Unknown commands resolve to Blocked (fail closed).
class RiskPolicy {
public:
  static std::expected<RiskPolicy, std::string> parse(std::string_view toml);
  static std::expected<RiskPolicy, std::string> load(const std::filesystem::path& file);

  // `args` are the command's arguments; the first one selects a choice-level override such as `session lock`.
  Tier tierFor(std::string_view command, std::span<const std::string> args = {}) const;
  bool knows(std::string_view command) const { return tiers_.contains(std::string(command)); }
  // Choice-level overrides for `command` (for example `session` -> {lock: act}), keyed by the first argument.
  std::map<std::string, Tier> choiceTiers(std::string_view command) const;
  std::vector<std::string> commands() const;

private:
  std::map<std::string, Tier, std::less<>> tiers_;
  std::map<std::string, Tier, std::less<>> choices_;
};

// Which tiers each client may use. Clients are identified by the service from the bus connection.
class GrantStore {
public:
  explicit GrantStore(std::filesystem::path file) : file_(std::move(file)) {}

  // A missing file is an empty store. A corrupt file is an error and grants nothing.
  std::expected<void, std::string> load();
  std::expected<void, std::string> save() const;

  // Blocked is never grantable; it is dropped from `tiers`.
  void set(const std::string& client, std::set<Tier> tiers);
  void revoke(const std::string& client);
  bool allows(const std::string& client, Tier tier) const;
  std::set<Tier> tiersFor(const std::string& client) const;
  std::vector<std::string> clients() const;

private:
  std::filesystem::path file_;
  std::map<std::string, std::set<Tier>> grants_;
};

// Stable client identity from the verified executable and command line of a bus peer. Programs run by an
// interpreter would otherwise all share the interpreter's grants, so the script path is part of the identity.
std::string clientIdentity(const std::string& exePath, const std::vector<std::string>& cmdline);

// The line sent to the shell's IPC socket: a fixed caller-cwd prefix, then the command and its arguments.
// The prefix matters: without it the server would read a stray record-separator byte inside model-supplied text
// as the cwd boundary and drop everything before it. Arguments containing that byte or NUL are refused.
std::expected<std::string, std::string> buildWireCommand(std::string_view tool, std::span<const std::string> argv,
                                                         std::span<const std::string> literal);

enum class Verdict { Allow, NeedsConfirmation, Deny };

struct Decision {
  Verdict verdict = Verdict::Deny;
  Tier tier = Tier::Blocked;
  bool tainted = false;  // the client read untrusted content since its last human confirmation
  std::string reason;
};

// Clients that received untrusted results carry a taint into their next confirmation.
class TaintTracker {
public:
  void markUntrusted(const std::string& client) { tainted_.insert(client); }
  void clear(const std::string& client) { tainted_.erase(client); }
  bool tainted(const std::string& client) const { return tainted_.contains(client); }

private:
  std::set<std::string> tainted_;
};

Decision evaluate(const RiskPolicy& policy, const GrantStore& grants, const TaintTracker& taint,
                  const std::string& client, std::string_view command, std::span<const std::string> args);

// Tiers whose results must be labelled untrusted for the receiving model.
bool resultIsUntrusted(Tier tier);

struct AuditRecord {
  std::string client;
  std::string tool;
  std::vector<std::string> args;
  Tier tier = Tier::Blocked;
  std::string decision;  // allowed | denied | confirmed | rejected | cancelled
  std::string outcome;   // empty until completion
  bool tainted = false;
};

// Append-only JSON lines, mode 0600. Arguments are truncated; secrets are the caller's to omit.
class AuditLog {
public:
  explicit AuditLog(std::filesystem::path file) : file_(std::move(file)) {}
  static constexpr std::size_t kMaxArgBytes = 120;
  std::expected<void, std::string> append(const AuditRecord& record,
                                          std::chrono::system_clock::time_point when = std::chrono::system_clock::now()) const;

private:
  std::filesystem::path file_;
};

}  // namespace helix::agent
