#pragma once

#include "desktop/agent_policy.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace helix::agent {

struct Request {
  std::string client;                // identity resolved by the service from the bus connection
  std::string tool;                  // a `noctalia msg` command from msg-risk.toml
  std::vector<std::string> argv;     // fixed arguments
  std::vector<std::string> literal;  // model-supplied text; sent after `--`
  std::string reason;                // shown in confirmation dialogs
};

using Done = std::function<void(bool ok, std::string resultJson)>;
// Runs the command in the shell and reports its text output.
using Executor = std::function<void(const std::string& command, const std::vector<std::string>& args,
                                     const std::vector<std::string>& literal, std::function<void(bool, std::string)> done)>;

// The stateful half of Agent1: decisions, confirmation queue, taint and audit. No D-Bus, no clocks of its
// own, no threads; the service calls it and delivers completions as signals.
class AgentBroker {
public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  static constexpr std::chrono::seconds kConfirmationDeadline{60};

  AgentBroker(RiskPolicy policy, GrantStore& grants, AuditLog audit, Executor execute, Clock clock);

  // Called when a confirmation is needed; the service forwards it to the registered confirmer.
  std::function<void(std::uint64_t operation, const nlohmann::json& details)> onConfirmationRequested;
  void setConfirmerPresent(bool present);  // false denies everything pending

  // Always returns an operation id; `done` is invoked once, possibly before submit returns.
  std::uint64_t submit(const Request& request, Done done);
  bool cancel(const std::string& client, std::uint64_t operation);
  bool confirm(std::uint64_t operation, bool approved);
  void expire();  // denies confirmations past their deadline
  std::size_t pending() const { return pending_.size(); }

  const RiskPolicy& policy() const { return policy_; }
  bool tainted(const std::string& client) const { return taint_.tainted(client); }

private:
  struct Pending {
    Request request;
    Tier tier;
    bool tainted;
    Done done;
    std::chrono::steady_clock::time_point deadline;
  };

  void run(const Request& request, Tier tier, bool tainted, Done done);
  void finish(const Request& request, Tier tier, bool tainted, const std::string& decision, const std::string& outcome);
  static std::string denial(const std::string& reason);

  RiskPolicy policy_;
  GrantStore& grants_;
  AuditLog audit_;
  Executor execute_;
  Clock clock_;
  TaintTracker taint_;
  bool confirmerPresent_ = false;
  std::uint64_t next_ = 0;
  std::map<std::uint64_t, Pending> pending_;
};

}  // namespace helix::agent
