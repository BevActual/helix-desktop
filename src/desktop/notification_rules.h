#pragma once
#include "desktop/protocol.h"
#include "config/config_types.h"
#include <regex>
#include <algorithm>
#include <set>
#include <stdexcept>
namespace helix {
inline Json notificationRule(const NotificationFilterConfig& rule) {
  Json result = {{"name", rule.name}, {"enabled", rule.enabled}, {"match", rule.match}, {"match_content", rule.matchContent},
    {"show_toast", rule.showToast}, {"save_history", rule.saveHistory}, {"play_sound", rule.playSound},
    {"bypass_dnd", rule.bypassDnd}, {"allow_permanent", rule.allowPermanent}, {"allowed_urgencies", rule.allowedUrgencies}};
  if (rule.overrideDuration) result["override_duration"] = *rule.overrideDuration;
  return result;
}
inline std::vector<NotificationFilterConfig> notificationRules(const Json& values) {
  if (!values.is_array() || values.size() > 256) throw std::invalid_argument("Expected at most 256 notification rules");
  std::vector<NotificationFilterConfig> result; std::set<std::string> names;
  for (const auto& value : values) {
    if (!value.is_object()) throw std::invalid_argument("Notification rules must be objects");
    NotificationFilterConfig rule;
    const auto text = [&](const char* key, std::size_t limit) {
      if (!value.contains(key) || !value.at(key).is_string()) throw std::invalid_argument("Notification rule text is invalid");
      auto content = value.at(key).get<std::string>();
      if (content.size() > limit || std::ranges::any_of(content, [](unsigned char c) { return c < 32 || c == 127; })) throw std::invalid_argument("Notification rule text is invalid");
      return content;
    };
    rule.name = text("name", 128); rule.match = text("match", 512); rule.matchContent = text("match_content", 1024);
    if (rule.name.empty() || !names.insert(rule.name).second) throw std::invalid_argument("Rule names must be unique and nonempty");
    const auto flag = [&](const char* key) {
      if (!value.contains(key) || !value.at(key).is_boolean()) throw std::invalid_argument("Notification rule flags must be boolean");
      return value.at(key).get<bool>();
    };
    rule.enabled = flag("enabled"); rule.showToast = flag("show_toast"); rule.saveHistory = flag("save_history");
    rule.playSound = flag("play_sound"); rule.bypassDnd = flag("bypass_dnd"); rule.allowPermanent = flag("allow_permanent");
    if (rule.enabled && rule.match.empty() && rule.matchContent.empty()) throw std::invalid_argument("Enabled rules need an application match or content expression");
    if (!rule.matchContent.empty()) {
      try { std::regex expression(rule.matchContent, std::regex_constants::ECMAScript | std::regex_constants::icase); }
      catch (const std::regex_error&) { throw std::invalid_argument("The content expression is invalid"); }
    }
    if (!value.contains("allowed_urgencies") || !value.at("allowed_urgencies").is_array()) throw std::invalid_argument("Invalid notification urgencies");
    std::set<std::string> urgencies;
    for (const auto& urgency : value.at("allowed_urgencies")) {
      if (!urgency.is_string() || (urgency != "low" && urgency != "normal" && urgency != "critical") || !urgencies.insert(urgency.get<std::string>()).second) throw std::invalid_argument("Invalid notification urgency");
      rule.allowedUrgencies.push_back(urgency.get<std::string>());
    }
    if (value.contains("override_duration")) {
      if (!value.at("override_duration").is_number_integer()) throw std::invalid_argument("Invalid notification duration");
      const auto duration = value.at("override_duration").get<std::int64_t>();
      if (duration < -1 || duration > 2147483647) throw std::invalid_argument("Invalid notification duration");
      rule.overrideDuration = static_cast<std::int32_t>(duration);
    }
    result.push_back(std::move(rule));
  }
  return result;
}
}
