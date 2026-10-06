#pragma once
#include "desktop_api/protocol.h"
#include <functional>
#include <memory>
#include <map>

namespace helix {
class DesktopClient {
public:
  DesktopClient();
  ~DesktopClient();
  DesktopClient(const DesktopClient&) = delete;
  DesktopClient& operator=(const DesktopClient&) = delete;
  // Used by shell adapters. GTK callers use callAsync to avoid blocking the UI.
  Json call(std::string_view domain, std::string_view method, GVariant* parameters = nullptr);
  void callAsync(std::string_view domain, std::string_view method, GVariant* parameters,
                 std::function<void(Json, std::string)> completion);
  bool available() const;
  using Observer = std::function<void(std::string_view, std::string_view, const Json&)>;
  unsigned observe(Observer observer);
  void unobserve(unsigned id);

private:
  GDBusConnection* m_bus = nullptr;
  guint m_subscription = 0;
  unsigned m_nextObserver = 1;
  std::map<unsigned, Observer> m_observers;

};
DesktopClient& desktopClient();
}
