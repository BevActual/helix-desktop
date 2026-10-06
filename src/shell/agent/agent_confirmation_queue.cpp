#include "shell/agent/agent_confirmation_queue.h"

#include "desktop/display_text.h"

#include <algorithm>

namespace helix::agent {

namespace {

std::vector<std::string> stringList(const nlohmann::json& details, const char* key, std::size_t maxItems, std::size_t maxChars) {
  std::vector<std::string> out;
  if (!details.contains(key) || !details.at(key).is_array()) return out;
  for (const auto& item : details.at(key)) {
    if (out.size() == maxItems) break;
    if (item.is_string()) out.push_back(sanitizeForDisplay(item.get<std::string>(), maxChars));
  }
  return out;
}

std::string stringField(const nlohmann::json& details, const char* key, std::size_t maxChars) {
  return details.contains(key) && details.at(key).is_string() ? sanitizeForDisplay(details.at(key).get<std::string>(), maxChars) : "";
}

}  // namespace

std::optional<ConfirmRequest> ConfirmRequest::fromJson(const nlohmann::json& details) {
  if (!details.is_object() || !details.contains("operation")) return std::nullopt;
  const auto& id = details.at("operation");
  const bool validId = id.is_number_unsigned() || (id.is_number_integer() && id.get<std::int64_t>() >= 0);
  if (!validId) return std::nullopt;
  ConfirmRequest request;
  request.operation = id.get<std::uint64_t>();
  request.client = stringField(details, "client", 240);
  if (request.client.empty()) return std::nullopt;
  // The name comes from the whole identity: shortening first would leave a fragment of the path as the "name".
  request.clientName = friendlyClientName(details.at("client").get<std::string>());
  request.kind = details.value("kind", std::string()) == "grant" ? Kind::Grant : Kind::Action;
  request.label = stringField(details, "label", 64);
  request.tool = stringField(details, "tool", 64);
  request.reason = stringField(details, "reason", 200);
  request.args = stringList(details, "args", 8, 120);
  request.tiers = stringList(details, "tiers", 5, 24);
  request.tainted = details.value("tainted", false);
  if (request.kind == Kind::Action && request.tool.empty()) return std::nullopt;
  if (request.kind == Kind::Grant && request.tiers.empty()) return std::nullopt;
  return request;
}

void ConfirmationQueue::enqueue(ConfirmRequest request) {
  const auto fromClient = static_cast<std::size_t>(std::ranges::count(pending_, request.client, &ConfirmRequest::client));
  if (pending_.size() >= kMaxPending || fromClient >= kMaxPerClient) {
    responder_(request.operation, false, {});  // refused without ever showing the user
    return;
  }
  const bool becomesFront = pending_.empty();
  pending_.push_back(std::move(request));
  if (becomesFront && onChanged) onChanged();
}

void ConfirmationQueue::answerFront(bool approved, std::vector<std::string> tiers) {
  if (pending_.empty()) return;
  const ConfirmRequest answered = std::move(pending_.front());
  pending_.pop_front();
  if (answered.kind == ConfirmRequest::Kind::Grant && approved) {
    // Only tiers that were actually requested can be granted, whatever the caller passes.
    std::vector<std::string> kept;
    for (const auto& tier : tiers)
      if (std::ranges::find(answered.tiers, tier) != answered.tiers.end()) kept.push_back(tier);
    if (kept.empty()) approved = false;
    tiers = std::move(kept);
  }
  responder_(answered.operation, approved, tiers);
  if (onChanged) onChanged();
}

void ConfirmationQueue::dropOperation(std::uint64_t operation) {
  const bool wasFront = !pending_.empty() && pending_.front().operation == operation;
  std::erase_if(pending_, [operation](const ConfirmRequest& request) { return request.operation == operation; });
  if (wasFront && onChanged) onChanged();
}

void ConfirmationQueue::denyAll() {
  auto waiting = std::move(pending_);
  pending_.clear();
  for (const auto& request : waiting) responder_(request.operation, false, {});
  if (onChanged) onChanged();
}

}  // namespace helix::agent
