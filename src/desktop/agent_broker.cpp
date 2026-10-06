#include "desktop/agent_broker.h"

namespace helix::agent {

AgentBroker::AgentBroker(RiskPolicy policy, GrantStore& grants, AuditLog audit, Executor execute, Clock clock)
    : policy_(std::move(policy)), grants_(grants), audit_(std::move(audit)), execute_(std::move(execute)),
      clock_(std::move(clock)) {}

std::string AgentBroker::denial(const std::string& reason) {
  return nlohmann::json{{"trust", "trusted"}, {"error", reason}}.dump();
}

void AgentBroker::setConfirmerPresent(bool present) {
  confirmerPresent_ = present;
  if (present) return;
  // Without a confirmer nobody can approve: deny what is waiting.
  auto waiting = std::move(pending_);
  pending_.clear();
  for (auto& [operation, entry] : waiting) {
    finish(entry.request, entry.tier, entry.tainted, "denied", "confirmer disconnected");
    entry.done(false, denial("the confirmation dialog is unavailable"));
  }
}

void AgentBroker::finish(const Request& request, Tier tier, bool tainted, const std::string& decision,
                         const std::string& outcome) {
  AuditRecord record;
  record.client = request.client;
  record.tool = request.tool;
  record.args = request.argv;
  record.args.insert(record.args.end(), request.literal.begin(), request.literal.end());
  record.tier = tier;
  record.decision = decision;
  record.outcome = outcome;
  record.tainted = tainted;
  (void)audit_.append(record);  // an unwritable audit log must not take the desktop down; see service logging
}

std::uint64_t AgentBroker::submit(const Request& request, Done done) {
  const std::uint64_t operation = ++next_;
  // The first token the command will see decides choice-level overrides, so evaluate fixed args and literals together.
  std::vector<std::string> all = request.argv;
  all.insert(all.end(), request.literal.begin(), request.literal.end());
  const Decision decision = evaluate(policy_, grants_, taint_, request.client, request.tool, all);

  switch (decision.verdict) {
    case Verdict::Deny:
      finish(request, decision.tier, decision.tainted, "denied", decision.reason);
      done(false, denial(decision.reason));
      return operation;
    case Verdict::Allow:
      run(request, decision.tier, decision.tainted, std::move(done));
      return operation;
    case Verdict::NeedsConfirmation:
      if (!confirmerPresent_ || !onConfirmationRequested) {
        finish(request, decision.tier, decision.tainted, "denied", "no confirmer");
        done(false, denial("this action needs confirmation and no confirmation dialog is available"));
        return operation;
      }
      pending_.emplace(operation, Pending{request, decision.tier, decision.tainted, std::move(done),
                                          clock_() + kConfirmationDeadline});
      onConfirmationRequested(operation, nlohmann::json{
        {"operation", operation}, {"client", request.client}, {"tool", request.tool}, {"args", all},
        {"reason", request.reason}, {"tainted", decision.tainted},
      });
      return operation;
  }
  return operation;
}

void AgentBroker::run(const Request& request, Tier tier, bool tainted, Done done) {
  finish(request, tier, tainted, tier == Tier::Confirm ? "confirmed" : "allowed", "");
  execute_(request.tool, request.argv, request.literal,
           [this, request, tier, done = std::move(done)](bool ok, std::string text) {
    const bool untrusted = resultIsUntrusted(tier);
    if (untrusted) taint_.markUntrusted(request.client);
    nlohmann::json result{{"trust", untrusted ? "untrusted" : "trusted"}};
    result[ok ? "content" : "error"] = std::move(text);
    done(ok, result.dump());
  });
}

bool AgentBroker::confirm(std::uint64_t operation, bool approved) {
  const auto found = pending_.find(operation);
  if (found == pending_.end()) return false;
  Pending entry = std::move(found->second);
  pending_.erase(found);
  if (!approved) {
    finish(entry.request, entry.tier, entry.tainted, "rejected", "");
    entry.done(false, denial("the user declined"));
    return true;
  }
  taint_.clear(entry.request.client);  // a human just looked at exactly this action
  run(entry.request, entry.tier, entry.tainted, std::move(entry.done));
  return true;
}

bool AgentBroker::cancel(const std::string& client, std::uint64_t operation) {
  const auto found = pending_.find(operation);
  if (found == pending_.end() || found->second.request.client != client) return false;
  Pending entry = std::move(found->second);
  pending_.erase(found);
  finish(entry.request, entry.tier, entry.tainted, "cancelled", "");
  entry.done(false, denial("cancelled"));
  return true;
}

void AgentBroker::expire() {
  const auto now = clock_();
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.deadline > now) { ++it; continue; }
    Pending entry = std::move(it->second);
    it = pending_.erase(it);
    finish(entry.request, entry.tier, entry.tainted, "denied", "confirmation timed out");
    entry.done(false, denial("the confirmation timed out"));
  }
}

}  // namespace helix::agent
