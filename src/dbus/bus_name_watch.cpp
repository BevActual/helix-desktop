#include "dbus/bus_name_watch.h"

#include <sdbus-c++/Message.h>
#include <utility>

namespace dbus_watch {

  namespace {

    constexpr std::string_view kRulePrefix =
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged',";

    sdbus::Slot watch(sdbus::IConnection& connection, std::string rule, NameOwnerHandler handler) {
      return connection.addMatch(
          std::move(rule),
          [handler = std::move(handler)](sdbus::Message msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            msg >> name >> oldOwner >> newOwner;
            handler(name, oldOwner, newOwner);
          },
          sdbus::return_slot
      );
    }

  } // namespace

  sdbus::Slot watchBusName(sdbus::IConnection& connection, std::string_view name, NameOwnerHandler handler) {
    return watch(connection, std::string(kRulePrefix) + "arg0='" + std::string(name) + "'", std::move(handler));
  }

  sdbus::Slot watchBusNamespace(sdbus::IConnection& connection, std::string_view ns, NameOwnerHandler handler) {
    return watch(connection, std::string(kRulePrefix) + "arg0namespace='" + std::string(ns) + "'", std::move(handler));
  }

} // namespace dbus_watch
