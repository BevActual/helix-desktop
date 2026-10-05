#pragma once
#include <gio/gio.h>

namespace helix::apps {
// Inspection must never create a primary GUI or require a display when no app is open.
inline void guardInspection(GApplication* app) {
  g_signal_connect(app, "handle-local-options", G_CALLBACK(+[](GApplication* application, GVariantDict* options, gpointer) -> gint {
    gboolean inspect = false;
    if (!g_variant_dict_lookup(options, "inspect-layout", "b", &inspect) || !inspect) return -1;
    GError* error = nullptr; auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    GVariant* result = bus ? g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
      g_variant_new("(s)", g_application_get_application_id(application)), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error) : nullptr;
    gboolean present = false; if (result) { g_variant_get(result, "(b)", &present); g_variant_unref(result); }
    if (error) g_error_free(error);
    if (bus) g_object_unref(bus);
    if (present) return -1;
    g_printerr("Open the application before inspecting its layout\n"); return 1;
  }), nullptr);
}
}
