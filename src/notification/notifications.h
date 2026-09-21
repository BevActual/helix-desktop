#pragma once

#include "notification/notification_manager.h"

#include <string>
#include <utility>

namespace notify {

  inline constexpr const char* kAppName = "Helix";

  inline NotificationManager*& instancePtr() {
    static NotificationManager* ptr = nullptr;
    return ptr;
  }

  inline void setInstance(NotificationManager* manager) { instancePtr() = manager; }

  inline NotificationManager* instance() { return instancePtr(); }

  inline void error(std::string app, std::string title, std::string body) {
    if (auto* m = instancePtr()) {
      m->addInternal(std::move(app), std::move(title), std::move(body), Urgency::Critical);
    }
  }

  inline void error(std::string title, std::string body) { error(kAppName, std::move(title), std::move(body)); }

  inline void info(std::string app, std::string title, std::string body) {
    if (auto* m = instancePtr()) {
      m->addInternal(std::move(app), std::move(title), std::move(body));
    }
  }

  inline void info(std::string title, std::string body) { info(kAppName, std::move(title), std::move(body)); }

} // namespace notify
