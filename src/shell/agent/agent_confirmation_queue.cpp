#include "shell/agent/agent_confirmation_queue.h"

#include <algorithm>

namespace helix::agent {

namespace {

// Number of bytes in the UTF-8 sequence starting at `lead`, or 0 if `lead` cannot start one.
std::size_t sequenceLength(unsigned char lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) return 2;
  if (lead >= 0xE0 && lead <= 0xEF) return 3;
  if (lead >= 0xF0 && lead <= 0xF4) return 4;
  return 0;
}

std::uint32_t decode(const std::string& text, std::size_t at, std::size_t length) {
  std::uint32_t value = length == 2 ? (static_cast<unsigned char>(text[at]) & 0x1F)
                      : length == 3 ? (static_cast<unsigned char>(text[at]) & 0x0F)
                                    : (static_cast<unsigned char>(text[at]) & 0x07);
  for (std::size_t i = 1; i < length; ++i) {
    const auto next = static_cast<unsigned char>(text[at + i]);
    if ((next & 0xC0) != 0x80) return 0xFFFFFFFF;
    value = (value << 6) | (next & 0x3F);
  }
  return value;
}

bool isHiddenFormatting(std::uint32_t cp) {
  return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064)
      || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF || (cp >= 0x80 && cp < 0xA0) || cp == 0x2028 || cp == 0x2029;
}

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

std::string sanitizeForDisplay(const std::string& text, std::size_t maxCodepoints) {
  std::string out;
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t length = sequenceLength(lead);
    std::uint32_t cp = 0xFFFFFFFF;
    if (length != 0 && i + length <= text.size()) {
      cp = length == 1 ? lead : decode(text, i, length);
      // Overlong forms, surrogates and out-of-range values are how hostile text hides from filters.
      const bool overlong = (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000);
      if (cp != 0xFFFFFFFF && (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) cp = 0xFFFFFFFF;
    } else {
      length = 1;
    }

    std::string piece;
    if (cp == 0xFFFFFFFF) {
      piece = "?";  // invalid UTF-8
    } else if (cp == '\n' || cp == '\r' || cp == '\t') {
      piece = " ";
    } else if (cp < 0x20 || cp == 0x7F || isHiddenFormatting(cp)) {
      piece.clear();
    } else {
      piece = text.substr(i, length);
    }
    i += length;
    if (piece.empty()) continue;
    if (count == maxCodepoints) { out += "…"; return out; }
    out += piece;
    ++count;
  }
  return out;
}

std::optional<ConfirmRequest> ConfirmRequest::fromJson(const nlohmann::json& details) {
  if (!details.is_object() || !details.contains("operation")) return std::nullopt;
  const auto& id = details.at("operation");
  const bool validId = id.is_number_unsigned() || (id.is_number_integer() && id.get<std::int64_t>() >= 0);
  if (!validId) return std::nullopt;
  ConfirmRequest request;
  request.operation = id.get<std::uint64_t>();
  request.client = stringField(details, "client", 120);
  if (request.client.empty()) return std::nullopt;
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
