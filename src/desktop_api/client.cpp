#include "desktop_api/client.h"
#include <stdexcept>

namespace helix {
DesktopClient::DesktopClient() {
  GError* error = nullptr;
  m_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (error) g_error_free(error);
  if (m_bus) m_subscription = g_dbus_connection_signal_subscribe(m_bus, busName, nullptr, nullptr,
      objectPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection*, const gchar*, const gchar*, const gchar* interface, const gchar* signal,
         GVariant* parameters, gpointer data) {
        auto& client = *static_cast<DesktopClient*>(data);
        auto observers = client.m_observers;
        Json value;
        try { value = fromVariant(parameters); } catch (...) { return; }
        for (const auto& [id, callback] : observers) {
          if (client.m_observers.contains(id)) callback(interface, signal, value);
        }
      }, this, nullptr);
}
DesktopClient::~DesktopClient() {
  if (m_bus && m_subscription) g_dbus_connection_signal_unsubscribe(m_bus, m_subscription);
  if (m_bus) g_object_unref(m_bus);
}
unsigned DesktopClient::observe(Observer observer) {
  const auto id = m_nextObserver++;
  m_observers.emplace(id, std::move(observer));
  return id;
}
void DesktopClient::unobserve(unsigned id) { m_observers.erase(id); }

Json DesktopClient::call(std::string_view domain, std::string_view method, GVariant* parameters) {
  if (!m_bus) throw std::runtime_error("Desktop service connection unavailable");
  GError* error = nullptr;
  auto* result = g_dbus_connection_call_sync(m_bus, busName, objectPath, interfaceFor(domain).c_str(),
      std::string(method).c_str(), parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
  if (!result) {
    std::string message = error ? error->message : "Desktop operation failed";
    if (error) g_error_free(error);
    throw std::runtime_error(message);
  }
  Json output = fromVariant(result);
  g_variant_unref(result);
  return output.empty() ? Json::object() : output.at(0);
}
void DesktopClient::callAsync(std::string_view domain, std::string_view method, GVariant* parameters,
                              std::function<void(Json, std::string)> completion) {
  if (!m_bus) { completion(Json::object(), "Desktop service connection unavailable"); return; }
  auto* callback = new std::function<void(Json, std::string)>(std::move(completion));
  g_dbus_connection_call(m_bus, busName, objectPath, interfaceFor(domain).c_str(), std::string(method).c_str(),
      parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::function<void(Json, std::string)>> cb(
            static_cast<std::function<void(Json, std::string)>*>(data));
        GError* error = nullptr;
        auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        Json value = Json::object();
        std::string message;
        if (error) { message = error->message; g_error_free(error); }
        if (reply) {
          try { auto tuple = fromVariant(reply); if (!tuple.empty()) value = tuple.at(0); }
          catch (const std::exception& exception) { message = exception.what(); }
          g_variant_unref(reply);
        }
        (*cb)(std::move(value), std::move(message));
      }, callback);
}
bool DesktopClient::available() const {
  if (!m_bus) return false;
  auto* reply = g_dbus_connection_call_sync(m_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", busName), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, 200, nullptr, nullptr);
  if (!reply) return false;
  gboolean owned = false;
  g_variant_get(reply, "(b)", &owned);
  g_variant_unref(reply);
  return owned;
}
DesktopClient& desktopClient() { static DesktopClient client; return client; }
}
