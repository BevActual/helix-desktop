#pragma once

#include "desktop/display_text.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace helix::agent {

// Everything in a confirmation request except the operation id and kind is influenced by the requesting agent, and
// so by whatever content it has read. It is sanitized before it can reach a dialog.
struct ConfirmRequest {
  enum class Kind { Action, Grant };

  std::uint64_t operation = 0;
  Kind kind = Kind::Action;
  std::string client;                 // verified identity from the service, shortened for display
  std::string clientName;             // short name derived from the full identity before shortening
  std::string label;                  // self-declared app name (grants)
  std::string tool;                   // actions
  std::string reason;                 // the agent's own words; displayed as a quotation, never as a fact
  std::vector<std::string> args;      // actions
  std::vector<std::string> tiers;     // grants: what is being asked for
  bool tainted = false;               // the client read private content since the user last approved something

  // Returns nullopt for a request that is malformed rather than a display problem.
  static std::optional<ConfirmRequest> fromJson(const nlohmann::json& details);
};

// Pending confirmations for the dialog. The service enforces deadlines; this queue limits what a flood can do
// to the user: a bounded total and a bounded share per client, refused immediately instead of piling up.
class ConfirmationQueue {
public:
  static constexpr std::size_t kMaxPending = 8;
  static constexpr std::size_t kMaxPerClient = 3;

  // `tiers` is meaningful for grants only: the tiers the user approved (a subset of what was asked).
  using Responder = std::function<void(std::uint64_t operation, bool approved, const std::vector<std::string>& tiers)>;

  explicit ConfirmationQueue(Responder responder) : responder_(std::move(responder)) {}

  std::function<void()> onChanged;  // the request shown to the user changed

  void enqueue(ConfirmRequest request);
  const ConfirmRequest* front() const { return pending_.empty() ? nullptr : &pending_.front(); }
  std::size_t size() const { return pending_.size(); }

  void answerFront(bool approved, std::vector<std::string> tiers = {});
  void dropOperation(std::uint64_t operation);  // the service answered or expired it
  void denyAll();

private:
  Responder responder_;
  std::deque<ConfirmRequest> pending_;
};

}  // namespace helix::agent
