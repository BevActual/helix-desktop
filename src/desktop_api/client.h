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
  // Whether the service owns its bus name. While it is there this is answered from a name watch (which needs the GLib main loop to
  // run), so polling callers do not each make a synchronous D-Bus call; it can be briefly stale after the service exits, and calls
  // then fail normally. When it is absent, or before the watch has reported, the bus is asked directly.
  bool available() const;
  // Called each time the service appears on the bus (including at start-up if it is already there). Replaces any earlier handler.
  void onAppeared(std::function<void()> handler);
  using Observer = std::function<void(std::string_view, std::string_view, const Json&)>;
  unsigned observe(Observer observer);
  void unobserve(unsigned id);

private:
  GDBusConnection* m_bus = nullptr;
  guint m_nameWatch = 0;
  bool m_ownerKnown = false;
  bool m_owned = false;
  std::function<void()> m_appeared;
  guint m_subscription = 0;
  unsigned m_nextObserver = 1;
  std::map<unsigned, Observer> m_observers;

};
DesktopClient& desktopClient();
}
