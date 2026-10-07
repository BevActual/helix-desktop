#include "desktop/agent_broker.h"

#include <algorithm>
#include <iterator>

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
  if (onAudit) onAudit();
}

std::uint64_t AgentBroker::submit(const Request& request, Done done) {
  const std::uint64_t operation = ++next_;
  if (!enabled_) {
    finish(request, Tier::Blocked, false, "denied", "agent disabled");
    done(false, denial("agent access is turned off"));
    return operation;
  }
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

std::uint64_t AgentBroker::requestAccess(const std::string& client, const std::string& label, std::set<Tier> tiers,
                                         Done done) {
  const std::uint64_t operation = ++next_;
  if (!enabled_) {
    Request request{client, "grant-request", {label}, {}, ""};
    finish(request, Tier::Confirm, false, "denied", "agent disabled");
    done(false, denial("agent access is turned off"));
    return operation;
  }
  tiers.erase(Tier::Blocked);
  Request request{client, "grant-request", {label}, {}, ""};
  for (const Tier tier : tiers) request.literal.emplace_back(tierName(tier));

  if (tiers.empty()) {
    finish(request, Tier::Confirm, false, "denied", "nothing grantable requested");
    done(false, denial("no grantable tiers were requested"));
    return operation;
  }
  const auto held = grants_.tiersFor(client);
  if (std::includes(held.begin(), held.end(), tiers.begin(), tiers.end())) {
    done(true, nlohmann::json{{"trust", "trusted"}, {"granted", request.literal}}.dump());
    return operation;
  }
  if (!confirmerPresent_ || !onConfirmationRequested) {
    finish(request, Tier::Confirm, false, "denied", "no confirmer");
    done(false, denial("access needs approval and no approval dialog is available"));
    return operation;
  }
  Pending entry{request, Tier::Confirm, false, std::move(done), clock_() + kConfirmationDeadline};
  entry.isGrant = true;
  entry.requested = tiers;
  pending_.emplace(operation, std::move(entry));
  onConfirmationRequested(operation, nlohmann::json{
    {"operation", operation}, {"kind", "grant"}, {"client", client}, {"label", label}, {"tiers", request.literal},
  });
  return operation;
}

void AgentBroker::grant(const Pending& entry, const std::optional<std::set<Tier>>& narrowed) {
  std::set<Tier> approved = entry.requested;
  if (narrowed) {
    std::set<Tier> kept;
    std::set_intersection(approved.begin(), approved.end(), narrowed->begin(), narrowed->end(),
                          std::inserter(kept, kept.begin()));
    approved = std::move(kept);
  }
  const auto before = grants_.tiersFor(entry.request.client);
  auto after = before;
  after.insert(approved.begin(), approved.end());
  grants_.set(entry.request.client, after);
  if (auto saved = grants_.save(); !saved) {
    grants_.set(entry.request.client, before);  // a grant that cannot be persisted must not apply
    finish(entry.request, entry.tier, false, "denied", saved.error());
    entry.done(false, denial("the grant could not be saved"));
    return;
  }
  nlohmann::json granted = nlohmann::json::array();
  for (const Tier tier : after) granted.push_back(std::string(tierName(tier)));
  finish(entry.request, entry.tier, false, "granted", granted.dump());
  entry.done(true, nlohmann::json{{"trust", "trusted"}, {"granted", granted}}.dump());
}

bool AgentBroker::confirm(std::uint64_t operation, bool approved, const std::optional<std::set<Tier>>& narrowed) {
  const auto found = pending_.find(operation);
  if (found == pending_.end()) return false;
  Pending entry = std::move(found->second);
  pending_.erase(found);
  if (!approved) {
    finish(entry.request, entry.tier, entry.tainted, "rejected", "");
    entry.done(false, denial("the user declined"));
    return true;
  }
  if (entry.isGrant) {
    grant(entry, narrowed);
    return true;
  }
  taint_.clear(entry.request.client);  // a human just looked at exactly this action
  run(entry.request, entry.tier, entry.tainted, std::move(entry.done));
  return true;
}

bool AgentBroker::cancel(const std::string& client, std::uint64_t operation) {
  const auto found = pending_.find(operation);
  if (found == pending_.end() || found->second.request.client != client) return false;
  return stop(operation);
}

bool AgentBroker::stop(std::uint64_t operation) {
  const auto found = pending_.find(operation);
  if (found == pending_.end()) return false;
  Pending entry = std::move(found->second);
  pending_.erase(found);
  finish(entry.request, entry.tier, entry.tainted, "cancelled", "stopped");
  if (onConfirmationEnded) onConfirmationEnded(operation);
  entry.done(false, denial("cancelled"));
  return true;
}

void AgentBroker::stopAll() {
  while (!pending_.empty()) stop(pending_.begin()->first);
}

nlohmann::json AgentBroker::listPending() const {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& [operation, entry] : pending_) {
    nlohmann::json row{{"operation", operation},
                       {"client", entry.request.client},
                       {"tool", entry.request.tool},
                       {"tier", std::string(tierName(entry.tier))},
                       {"kind", entry.isGrant ? "grant" : "action"},
                       {"tainted", entry.tainted}};
    if (!entry.request.reason.empty()) row["reason"] = entry.request.reason;
    out.push_back(std::move(row));
  }
  return out;
}

void AgentBroker::setEnabled(bool enabled) {
  enabled_ = enabled;
  if (!enabled_) stopAll();
}

void AgentBroker::expire() {
  const auto now = clock_();
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.deadline > now) { ++it; continue; }
    const std::uint64_t operation = it->first;
    Pending entry = std::move(it->second);
    it = pending_.erase(it);
    finish(entry.request, entry.tier, entry.tainted, "denied", "confirmation timed out");
    if (onConfirmationEnded) onConfirmationEnded(operation);
    entry.done(false, denial("the confirmation timed out"));
  }
}

void AgentBroker::recordGrantChange(const std::string& client, const std::string& change,
                                    const std::vector<std::string>& tiers) {
  AuditRecord record;
  record.client = client;
  record.tool = "grant-change";
  record.args = tiers;
  record.tier = Tier::Confirm;  // only the user's own manager can do this
  record.decision = "changed";
  record.outcome = change;
  (void)audit_.append(record);
}

}  // namespace helix::agent
