#pragma once

#include <functional>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/TypeTraits.h>
#include <string>
#include <string_view>

namespace dbus_watch {

  using NameOwnerHandler =
      std::function<void(const std::string& name, const std::string& oldOwner, const std::string& newOwner)>;

  // NameOwnerChanged filtered by the bus daemon on arg0, so the shell only wakes for the
  // one name it cares about. An unfiltered `uponSignal("NameOwnerChanged")` delivers every
  // connect and disconnect on the bus, and browsers open short-lived connections tens of
  // times a second. The returned Slot removes the match when dropped.
  [[nodiscard]] sdbus::Slot watchBusName(sdbus::IConnection& connection, std::string_view name, NameOwnerHandler handler);

  // Same, for a dotted namespace: matches `ns` itself and every `ns.<suffix>` (MPRIS players).
  [[nodiscard]] sdbus::Slot
  watchBusNamespace(sdbus::IConnection& connection, std::string_view ns, NameOwnerHandler handler);

} // namespace dbus_watch
