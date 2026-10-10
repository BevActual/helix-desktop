#pragma once

#include "desktop/agent_policy.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix::agent {

struct Request {
  std::string client;                // identity resolved by the service from the bus connection
  std::string tool;                  // a `helix-panel` command from msg-risk.toml
  std::vector<std::string> argv;     // fixed arguments
  std::vector<std::string> literal;  // model-supplied text; sent after `--`
  std::string reason;                // shown in confirmation dialogs
};

using Done = std::function<void(bool ok, std::string resultJson)>;
// Runs the command in the shell and reports its text output.
using Executor = std::function<void(const std::string& command, const std::vector<std::string>& args,
                                     const std::vector<std::string>& literal, std::function<void(bool, std::string)> done)>;

// The stateful half of Agent Surface: decisions, confirmation queue, taint and audit. No D-Bus, no clocks of its
// own, no threads; the service calls it and delivers completions as signals.
class AgentBroker {
public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  static constexpr std::chrono::seconds kConfirmationDeadline{60};

  AgentBroker(RiskPolicy policy, GrantStore& grants, AuditLog audit, Executor execute, Clock clock);

  // Called when a confirmation is needed; the service forwards it to the registered confirmer.
  std::function<void(std::uint64_t operation, const nlohmann::json& details)> onConfirmationRequested;
  // Called when a pending confirmation ends without the confirmer answering it (the requester cancelled or left, or
  // the deadline passed), so whoever is showing it can stop.
  std::function<void(std::uint64_t operation)> onConfirmationEnded;
  std::function<void()> onAudit;  // a record was appended: the activity view should refresh
  void setConfirmerPresent(bool present);  // false denies everything pending

  // Always returns an operation id; `done` is invoked once, possibly before submit returns.
  std::uint64_t submit(const Request& request, Done done);
  // First-use consent: asks the confirmer to grant `tiers` to `client`. Already-held tiers need no dialog.
  // `blocked` is dropped from the request. Completes with the tiers the client now holds.
  std::uint64_t requestAccess(const std::string& client, const std::string& label, std::set<Tier> tiers, Done done);
  bool cancel(const std::string& client, std::uint64_t operation);
  // Shell stop: cancel any pending operation without checking the requester. Does not revoke grants.
  bool stop(std::uint64_t operation);
  void stopAll();
  // For grant requests `narrowed` lets the user approve fewer tiers than were asked; extras are ignored.
  bool confirm(std::uint64_t operation, bool approved, const std::optional<std::set<Tier>>& narrowed = std::nullopt);
  void expire();  // denies confirmations past their deadline
  // Records a change the user made to a client's grants (through the manager), so the log shows who widened what.
  void recordGrantChange(const std::string& client, const std::string& change, const std::vector<std::string>& tiers);
  std::size_t pending() const { return pending_.size(); }
  nlohmann::json listPending() const;

  // Kill switch / safe mode. When false, submit and requestAccess fail closed; pending is stopped.
  bool enabled() const { return enabled_; }
  void setEnabled(bool enabled);

  const RiskPolicy& policy() const { return policy_; }
  bool tainted(const std::string& client) const { return taint_.tainted(client); }

private:
  struct Pending {
    Request request;
    Tier tier;
    bool tainted;
    Done done;
    std::chrono::steady_clock::time_point deadline;
    bool isGrant = false;
    std::set<Tier> requested;  // grant requests only
  };

  void run(const Request& request, Tier tier, bool tainted, Done done);
  void grant(const Pending& entry, const std::optional<std::set<Tier>>& narrowed);
  void finish(const Request& request, Tier tier, bool tainted, const std::string& decision, const std::string& outcome);
  static std::string denial(const std::string& reason);

  RiskPolicy policy_;
  GrantStore& grants_;
  AuditLog audit_;
  Executor execute_;
  Clock clock_;
  TaintTracker taint_;
  bool confirmerPresent_ = false;
  bool enabled_ = true;
  std::uint64_t next_ = 0;
  std::map<std::uint64_t, Pending> pending_;
};

}  // namespace helix::agent
