#include "desktop/notification_rules.h"
#include "desktop_api/client.h"

#include <cassert>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using helix::Json;
namespace {
  struct Child {
    pid_t pid = -1;
    ~Child() { stop(); }
    void stop() {
      if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        pid = -1;
      }
    }
    void start(const char* executable, const char* argument = nullptr) {
      pid = fork();
      if (pid == 0) {
        execl(executable, executable, argument, nullptr);
        _exit(127);
      }
      assert(pid > 0);
    }
  };
  bool rejects(helix::DesktopClient& client, const char* domain, const char* method, GVariant* args) {
    try {
      client.call(domain, method, args);
      return false;
    } catch (...) {
      return true;
    }
  }
  void waitForService(helix::DesktopClient& client) {
    for (unsigned i = 0; i < 200; ++i) {
      try {
        if (client.available() && client.call("configuration", "GetSnapshot").contains("available"))
          return;
      } catch (...) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    assert(false && "Desktop service did not become ready");
  }
  Json setting(helix::DesktopClient& client, const std::string& id) {
    auto values = client.call("configuration", "GetSettings");
    for (const auto& value : values)
      if (value.at("id") == id)
        return value;
    assert(false);
    return {};
  }
} // namespace
// A controllable BlueZ replacement on the private test bus. No hardware access.
int fakeBluez() {
  auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  assert(bus);
  const char* xml = R"(<node>
    <interface name="org.freedesktop.DBus.ObjectManager"><method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method></interface>
    <interface name="org.bluez.AgentManager1"><method name="RegisterAgent"><arg type="o" direction="in"/><arg type="s" direction="in"/></method><method name="RequestDefaultAgent"><arg type="o" direction="in"/></method><method name="UnregisterAgent"><arg type="o" direction="in"/></method></interface>
    <interface name="org.bluez.Device1"><method name="Pair"/><method name="CancelPairing"/><method name="GetCancelCount"><arg type="u" direction="out"/></method></interface>
    </node>)";
  auto* info = g_dbus_node_info_new_for_xml(xml, nullptr);
  assert(info);
  unsigned cancels = 0;
  const GDBusInterfaceVTable table = {
      +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant*,
          GDBusMethodInvocation* invocation, gpointer data) {
        auto& cancels = *static_cast<unsigned*>(data);
        if (std::string(method) == "GetManagedObjects") {
          const char* objects =
              "{'/org/bluez/hci0': {'org.bluez.Adapter1': {'Powered': <false>, 'Alias': <'Test Adapter'>}}, "
              "'/org/bluez/hci0/dev_FAKE': {'org.bluez.Device1': {'Address': <'00:00:00:00:00:01'>, 'Alias': <'Test "
              "Device'>, 'Paired': <false>, 'Connected': <false>}}}";
          auto* value = g_variant_parse(G_VARIANT_TYPE("a{oa{sa{sv}}}"), objects, nullptr, nullptr, nullptr);
          assert(value);
          g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{oa{sa{sv}}})", value));
        } else if (std::string(method) == "GetCancelCount")
          g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", cancels));
        else {
          if (std::string(method) == "CancelPairing")
            ++cancels;
          g_dbus_method_invocation_return_value(invocation, nullptr);
        }
      },
      nullptr,
      nullptr,
      {nullptr}
  };
  assert(g_dbus_connection_register_object(bus, "/", info->interfaces[0], &table, &cancels, nullptr, nullptr));
  assert(g_dbus_connection_register_object(bus, "/org/bluez", info->interfaces[1], &table, &cancels, nullptr, nullptr));
  assert(g_dbus_connection_register_object(
      bus, "/org/bluez/hci0/dev_FAKE", info->interfaces[2], &table, &cancels, nullptr, nullptr
  ));
  g_bus_own_name_on_connection(bus, "org.bluez", G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);
  g_main_loop_run(g_main_loop_new(nullptr, false));
  return 0;
}
int main(int argc, char** argv) {
  assert(argc == 2);
  if (std::string(argv[1]) == "--fake-bluez")
    return fakeBluez();
  const auto root = std::filesystem::temp_directory_path() / ("helix-service-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(root / "config/helix");
  const auto source = root / "config/helix/config.toml";
  {
    std::ofstream file(source);
    file << "[theme]\nmode = \"dark\"\n[control_center]\nhidden_tabs = [\"plugin:test-hidden\", \"ai-usage\", "
            "\"screenshot\", \"screen-record\"]\n[[control_center.shortcuts]]\ntype = \"author/plugin:pin\"\nsize = "
            "\"wide\"\n";
  }
  setenv("HELIX_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  setenv("GSETTINGS_BACKEND", "memory", 1);
  // Never inspect or change the user's hardware in this integration test.
  setenv("DBUS_SYSTEM_BUS_ADDRESS", getenv("DBUS_SESSION_BUS_ADDRESS"), 1);
  unsetenv("WAYLAND_DISPLAY");
  unsetenv("DISPLAY");
  unsetenv("NIRI_SOCKET");
  setenv("PIPEWIRE_REMOTE", "helix-test-no-audio", 1);
  Child service;
  service.start(argv[1]);
  helix::DesktopClient client;
  waitForService(client);
  auto snapshot = client.call("configuration", "GetSnapshot");
  assert(!snapshot.at("helix-desktop").get<bool>());
  const auto displayInventorySnapshot = client.call("displays", "GetSnapshot");
  assert(!displayInventorySnapshot.at("available").get<bool>() && displayInventorySnapshot.at("outputs").empty());
  assert(rejects(client, "displays", "Action", g_variant_new("(s)", "unsupported")));
  const auto appearance = client.call("appearance", "GetSnapshot");
  assert(appearance.at("wallpaper") == "");
  const auto& palettePreviews = appearance.at("palette-previews");
  assert(palettePreviews.is_array() && palettePreviews.size() >= 10);
  for (const auto& preview : palettePreviews) {
    assert(preview.at("source").is_string() && preview.at("name").is_string());
    for (const char* mode : {"dark", "light"}) {
      const auto& variant = preview.at(mode);
      assert(variant.at("surface").is_number_unsigned());
      assert(variant.at("accents").is_array() && !variant.at("accents").empty());
    }
  }
  assert(!client.call("appearance", "GetSnapshot").at("wallhaven-online").get<bool>());
  assert(rejects(client, "appearance", "SearchWallhaven", g_variant_new("(@a{sv})", helix::toVariant(Json::object()))));
  assert(!snapshot.at("network").get<bool>());
  assert(snapshot.at("system").at("os").is_string());
  assert(snapshot.at("system").at("architecture").is_string());
  for (const auto& item : client.call("configuration", "GetSettings"))
    // privacy-security carries only the read-only Mobius routing mirror (ai.*), which exists for search.
    assert(
        item.at("page") != "notifications"
        && item.at("page") != "accessibility"
        && item.at("page") != "keyboard"
        && item.at("page") != "system"
        && item.at("page") != "power"
        && item.at("page") != "desktop-dock"
        && (item.at("page") != "privacy-security" || item.at("id").get<std::string>().starts_with("ai."))
    );
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "notification.show_actions", g_variant_new_boolean(false))
  ));
  assert(!snapshot.at("bluetooth").get<bool>());
  Child bluez;
  bluez.start(argv[0], "--fake-bluez");
  auto waitBluetooth = [&](bool available) {
    for (unsigned i = 0; i < 240; ++i) {
      if (client.call("bluetooth", "GetSnapshot").value("available", false) == available)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    assert(false && "Bluetooth backend capability did not recover");
  };
  waitBluetooth(true);
  std::map<std::uint64_t, Json> completions;
  const auto observer = client.observe([&](std::string_view interface, std::string_view signal, const Json& value) {
    if (interface == helix::bluetoothInterface && signal == "Completed")
      completions[value.at(0).get<std::uint64_t>()] = value;
  });
  const auto waitCompletion = [&](std::uint64_t id) {
    for (unsigned i = 0; i < 80 && !completions.contains(id); ++i) {
      while (g_main_context_pending(nullptr))
        g_main_context_iteration(nullptr, false);
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    assert(completions.contains(id));
    return completions.at(id);
  };
  const auto operation =
      client
          .call(
              "bluetooth", "Action",
              g_variant_new("(s@a{sv})", "pair", helix::toVariant({{"path", "/org/bluez/hci0/dev_FAKE"}}))
          )
          .get<std::uint64_t>();
  auto* other = g_dbus_connection_new_for_address_sync(
      getenv("DBUS_SESSION_BUS_ADDRESS"),
      GDBusConnectionFlags(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
      ),
      nullptr, nullptr, nullptr
  );
  GError* denied = nullptr;
  auto* rejected = g_dbus_connection_call_sync(
      other, helix::busName, helix::objectPath, helix::bluetoothInterface, "Cancel", g_variant_new("(t)", operation),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &denied
  );
  assert(!rejected && denied);
  g_error_free(denied);
  g_dbus_connection_close_sync(other, nullptr, nullptr);
  g_object_unref(other);
  client.call("bluetooth", "Cancel", g_variant_new("(t)", operation));
  assert(rejects(client, "bluetooth", "Cancel", g_variant_new("(t)", operation)));
  assert(waitCompletion(operation).at(1) == false);
  assert(completions.at(operation).at(2) == "Cancelled");
  auto* testBus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  auto* count = g_dbus_connection_call_sync(
      testBus, "org.bluez", "/org/bluez/hci0/dev_FAKE", "org.bluez.Device1", "GetCancelCount", nullptr, nullptr,
      G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr
  );
  assert(count && helix::fromVariant(count).at(0) == 1);
  g_variant_unref(count);
  g_object_unref(testBus);
  const auto interrupted =
      client
          .call(
              "bluetooth", "Action",
              g_variant_new("(s@a{sv})", "pair", helix::toVariant({{"path", "/org/bluez/hci0/dev_FAKE"}}))
          )
          .get<std::uint64_t>();
  bluez.stop();
  waitBluetooth(false);
  assert(waitCompletion(interrupted).at(1) == false);
  assert(completions.at(interrupted).at(2) == "Device service disconnected");
  bluez.start(argv[0], "--fake-bluez");
  waitBluetooth(true);
  bluez.stop();
  waitBluetooth(false);
  client.unobserve(observer);
  assert(rejects(client, "audio", "StartInputMeter", g_variant_new("(u)", 999u)));
  client.call("audio", "StopInputMeter");
  assert(setting(client, "theme.mode").at("value") == "dark");
  client.call("configuration", "Update", g_variant_new("(sv)", "theme.mode", g_variant_new_string("light")));
  assert(setting(client, "theme.mode").at("value") == "light");
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "theme.mode", g_variant_new_string("invalid")))
  );
  assert(rejects(client, "configuration", "Update", g_variant_new("(sv)", "theme.mode", g_variant_new_boolean(true))));
  assert(rejects(client, "configuration", "Update", g_variant_new("(sv)", "missing", g_variant_new_string("value"))));
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "dock.enabled", g_variant_new_boolean(true)))
  );
  assert(rejects(
      client, "network", "Action",
      g_variant_new(
          "(s@a{sv})", "connect", helix::toVariant({{"path", "/missing"}, {"secret", "never-log-this-password"}})
      )
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "theme.mode"));
  assert(setting(client, "theme.mode").at("value") == "dark");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "wallpaper.enabled", g_variant_new_boolean(false))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "dock.launcher_position", g_variant_new_string("start"))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "control_center.width", g_variant_new_int64(900))
  ));
  client.call("configuration", "RegisterDesktop");
  client.call(
      "configuration", "RegisterCapabilities", g_variant_new("(@a{sv})", helix::toVariant({{"gamma-control", true}}))
  );
  assert(setting(client, "nightlight.enabled").at("page") == "displays");
  const auto originalDay = setting(client, "nightlight.temperature_day").at("value");
  const auto originalNight = setting(client, "nightlight.temperature_night").at("value");
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "nightlight.temperature_night", g_variant_new_int64(9000))
  );
  assert(setting(client, "nightlight.temperature_night").at("value") == 9000);
  assert(setting(client, "nightlight.temperature_day").at("value").get<int>() >= 9100);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "nightlight.temperature_day", g_variant_new_int64(2000))
  );
  assert(setting(client, "nightlight.temperature_day").at("value") == 2000);
  assert(setting(client, "nightlight.temperature_night").at("value").get<int>() <= 1900);
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "nightlight.temperature_day", g_variant_new_int64(1000))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "nightlight.temperature_night"));
  assert(setting(client, "nightlight.temperature_day").at("value") == originalDay);
  assert(setting(client, "nightlight.temperature_night").at("value") == originalNight);
  assert(rejects(
      client, "configuration", "RegisterCapabilities", g_variant_new("(@a{sv})", helix::toVariant({{"unknown", true}}))
  ));
  client.call(
      "configuration", "RegisterCapabilities", g_variant_new("(@a{sv})", helix::toVariant({{"gamma-control", false}}))
  );
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "nightlight.enabled", g_variant_new_boolean(true))
  ));
  assert(setting(client, "shell.app_icon_color").at("page") == "appearance");
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.app_icon_color", g_variant_new_string("#336699"))
  );
  assert(setting(client, "shell.app_icon_color").at("value") == "#336699");
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.app_icon_color", g_variant_new_string("primary"))
  );
  assert(setting(client, "shell.app_icon_color").at("value") == "primary");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.app_icon_color", g_variant_new_string("not-a-color"))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.app_icon_color"));
  assert(setting(client, "shell.app_icon_color").at("value") == "");
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "hooks.started", g_variant_new_string("notify-send hello"))
  );
  assert(setting(client, "hooks.started").at("value") == "notify-send hello");
  assert(setting(client, "hooks.started").at("page") == "desktop-dock");
  client.call("configuration", "Reset", g_variant_new("(s)", "hooks.started"));
  assert(setting(client, "hooks.started").at("value") == "");
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.panel.launcher_placement", g_variant_new_string("attached"))
  );
  assert(setting(client, "shell.panel.launcher_placement").at("value") == "attached");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.panel.launcher_placement", g_variant_new_string("stacked"))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.panel.launcher_placement"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "desktop_widgets.enabled", g_variant_new_boolean(false))
  );
  assert(setting(client, "desktop_widgets.enabled").at("value") == false);
  client.call("configuration", "Reset", g_variant_new("(s)", "desktop_widgets.enabled"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.screen_corners.enabled", g_variant_new_boolean(true))
  );
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.screen_corners.size", g_variant_new_int64(48)));
  assert(setting(client, "shell.screen_corners.size").at("value") == 48);
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "shell.screen_corners.size", g_variant_new_int64(0))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.screen_corners.size"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.screen_corners.enabled"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.panel.floating_layer", g_variant_new_string("top"))
  );
  assert(setting(client, "shell.panel.floating_layer").at("value") == "top");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.panel.floating_layer", g_variant_new_string("bottom"))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.panel.floating_layer"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.panel_anchor_bar", g_variant_new_string("default"))
  );
  assert(setting(client, "shell.panel_anchor_bar").at("value") == "default");
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.panel_anchor_bar"));
  assert(setting(client, "shell.panel_anchor_bar").at("value") == "");
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.panel_anchor_bar", g_variant_new_string("")));
  assert(setting(client, "shell.panel_anchor_bar").at("value") == "");
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.layer", g_variant_new_string("overlay")));
  assert(setting(client, "dock.layer").at("value") == "overlay");
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.background_opacity", g_variant_new_double(0.5)));
  assert(setting(client, "dock.background_opacity").at("value") == 0.5);
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.radius", g_variant_new_int64(24)));
  assert(setting(client, "dock.radius").at("value") == 24);
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.active_scale", g_variant_new_double(1.25)));
  assert(setting(client, "dock.active_scale").at("value") == 1.25);
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "dock.active_scale", g_variant_new_double(2.0)))
  );
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.layer"));
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.background_opacity"));
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.radius"));
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.active_scale"));
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:layer", g_variant_new_string("overlay")));
  assert(setting(client, "bar:default:layer").at("value") == "overlay");
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:scale", g_variant_new_double(1.25)));
  assert(setting(client, "bar:default:scale").at("value") == 1.25);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "bar:default:background_opacity", g_variant_new_double(0.75))
  );
  assert(setting(client, "bar:default:background_opacity").at("value") == 0.75);
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:margin_ends", g_variant_new_int64(40)));
  assert(setting(client, "bar:default:margin_ends").at("value") == 40);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "bar:default:contact_shadow", g_variant_new_boolean(true))
  );
  assert(setting(client, "bar:default:contact_shadow").at("value") == true);
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "bar:default:scale", g_variant_new_double(0.1)))
  );
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:layer"));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:scale"));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:background_opacity"));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:margin_ends"));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:contact_shadow"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.launcher.auto_paste", g_variant_new_string("ctrl_v"))
  );
  assert(setting(client, "shell.launcher.auto_paste").at("value") == "ctrl_v");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.launcher.auto_paste", g_variant_new_string("enter"))
  ));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.launcher.provider_prefix", g_variant_new_string(">"))
  );
  assert(setting(client, "shell.launcher.provider_prefix").at("value") == ">");
  {
    const std::string tooLong(17, 'x');
    assert(rejects(
        client, "configuration", "Update",
        g_variant_new("(sv)", "shell.launcher.provider_prefix", g_variant_new_string(tooLong.c_str()))
    ));
  }
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.launcher.fetch_exchange_rates", g_variant_new_boolean(false))
  );
  assert(setting(client, "shell.launcher.fetch_exchange_rates").at("value") == false);
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.launcher.auto_paste"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.launcher.provider_prefix"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.launcher.fetch_exchange_rates"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.window_switcher.mru", g_variant_new_boolean(true))
  );
  assert(setting(client, "shell.window_switcher.mru").at("value") == true);
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.window_switcher.mru"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.session.show_shortcuts", g_variant_new_boolean(false))
  );
  assert(setting(client, "shell.session.show_shortcuts").at("value") == false);
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.session.show_shortcuts"));
  client.call("configuration", "Update", g_variant_new("(sv)", "osd.background_opacity", g_variant_new_double(0.5)));
  assert(setting(client, "osd.background_opacity").at("value") == 0.5);
  client.call("configuration", "Update", g_variant_new("(sv)", "osd.border", g_variant_new_boolean(false)));
  assert(setting(client, "osd.border").at("value") == false);
  client.call("configuration", "Update", g_variant_new("(sv)", "osd.offset_x", g_variant_new_int64(40)));
  assert(setting(client, "osd.offset_x").at("value") == 40);
  client.call("configuration", "Update", g_variant_new("(sv)", "osd.kinds.caffeine", g_variant_new_boolean(false)));
  assert(setting(client, "osd.kinds.caffeine").at("value") == false);
  client.call("configuration", "Reset", g_variant_new("(s)", "osd.background_opacity"));
  client.call("configuration", "Reset", g_variant_new("(s)", "osd.border"));
  client.call("configuration", "Reset", g_variant_new("(s)", "osd.offset_x"));
  client.call("configuration", "Reset", g_variant_new("(s)", "osd.kinds.caffeine"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "lockscreen_widgets.enabled", g_variant_new_boolean(true))
  );
  assert(setting(client, "lockscreen_widgets.enabled").at("value") == true);
  client.call("configuration", "Reset", g_variant_new("(s)", "lockscreen_widgets.enabled"));
  assert(setting(client, "lockscreen_widgets.enabled").at("value") == false);
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.launcher_icon", g_variant_new_string("apps")));
  assert(setting(client, "dock.launcher_icon").at("value") == "apps");
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "dock.launcher_icon", g_variant_new_string("")))
  );
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.launcher_custom_image", g_variant_new_string("")));
  assert(setting(client, "dock.launcher_custom_image").at("value") == "");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "dock.launcher_custom_image", g_variant_new_string("/no/such/image.png"))
  ));
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "dock.launcher_custom_image_colorize", g_variant_new_boolean(true))
  );
  assert(setting(client, "dock.launcher_custom_image_colorize").at("value") == true);
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.launcher_icon"));
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.launcher_custom_image_colorize"));
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:font_scale", g_variant_new_double(1.25)));
  assert(setting(client, "bar:default:font_scale").at("value") == 1.25);
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:panel_overlap", g_variant_new_int64(2)));
  assert(setting(client, "bar:default:panel_overlap").at("value") == 2);
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "bar:default:panel_overlap", g_variant_new_int64(4))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:font_scale"));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:panel_overlap"));
  client.call("configuration", "Update", g_variant_new("(sv)", "lockscreen.enabled", g_variant_new_boolean(false)));
  assert(setting(client, "lockscreen.enabled").at("value") == false);
  client.call("configuration", "Reset", g_variant_new("(s)", "lockscreen.enabled"));
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.polkit_agent", g_variant_new_boolean(true)));
  assert(setting(client, "shell.polkit_agent").at("value") == true);
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.polkit_agent"));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.screenshot.freeze_screen", g_variant_new_boolean(false))
  );
  assert(setting(client, "shell.screenshot.freeze_screen").at("value") == false);
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.screenshot.copy_to_clipboard", g_variant_new_boolean(false))
  );
  assert(setting(client, "shell.screenshot.copy_to_clipboard").at("value") == false);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.screen_record.replay_duration", g_variant_new_int64(60))
  );
  assert(setting(client, "shell.screen_record.replay_duration").at("value") == 60);
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.screen_record.replay_duration", g_variant_new_int64(1))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.screenshot.freeze_screen"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.screenshot.copy_to_clipboard"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.screen_record.replay_duration"));
  client.call("configuration", "Update", g_variant_new("(sv)", "theme.shell_mode", g_variant_new_string("dark")));
  assert(setting(client, "theme.shell_mode").at("value") == "dark");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "theme.shell_mode", g_variant_new_string("sepia"))
  ));
  client.call("configuration", "Update", g_variant_new("(sv)", "backdrop.enabled", g_variant_new_boolean(true)));
  assert(setting(client, "backdrop.enabled").at("value") == true);
  client.call("configuration", "Update", g_variant_new("(sv)", "location.auto_locate", g_variant_new_boolean(true)));
  assert(setting(client, "location.auto_locate").at("value") == true);
  client.call("configuration", "Update", g_variant_new("(sv)", "location.sunrise", g_variant_new_string("06:30")));
  assert(setting(client, "location.sunrise").at("value") == "06:30");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "location.sunrise", g_variant_new_string("6:30"))
  ));
  client.call("configuration", "Update", g_variant_new("(sv)", "weather.unit", g_variant_new_string("imperial")));
  assert(setting(client, "weather.unit").at("value") == "imperial");
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "weather.unit", g_variant_new_string("kelvin")))
  );
  client.call("configuration", "Update", g_variant_new("(sv)", "audio.enable_overdrive", g_variant_new_boolean(true)));
  assert(setting(client, "audio.enable_overdrive").at("value") == true);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "brightness.enable_ddcutil", g_variant_new_boolean(true))
  );
  assert(setting(client, "brightness.enable_ddcutil").at("value") == true);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "control_center.network.wifi_band", g_variant_new_string("5"))
  );
  assert(setting(client, "control_center.network.wifi_band").at("value") == "5");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "control_center.network.wifi_band", g_variant_new_string("60"))
  ));
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "control_center.bluetooth.hide_unnamed", g_variant_new_boolean(true))
  );
  assert(setting(client, "control_center.bluetooth.hide_unnamed").at("value") == true);
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.time_format", g_variant_new_string("%H:%M")));
  assert(setting(client, "shell.time_format").at("value") == "%H:%M");
  client.call("configuration", "Update", g_variant_new("(sv)", "calendar.enabled", g_variant_new_boolean(true)));
  assert(setting(client, "calendar.enabled").at("value") == true);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "notification.filter_replace", g_variant_new_boolean(true))
  );
  assert(setting(client, "notification.filter_replace").at("value") == true);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "tailscale.notify_on_peer_change", g_variant_new_boolean(true))
  );
  assert(setting(client, "tailscale.notify_on_peer_change").at("value") == true);
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.border", g_variant_new_string("primary")));
  assert(setting(client, "dock.border").at("value") == "primary");
  {
    auto transitions = setting(client, "wallpaper.transition");
    assert(transitions.at("type") == "transition-list");
    assert(rejects(
        client, "configuration", "Update",
        g_variant_new("(sv)", "wallpaper.transition", helix::toVariant(transitions.at("value")))
    ));
    Json selected = Json::array({"fade", "wipe"});
    client.call(
        "configuration", "UpdateOrderedList",
        g_variant_new(
            "(s@av@av)", "wallpaper.transition", helix::toVariant(transitions.at("value")), helix::toVariant(selected)
        )
    );
    assert(setting(client, "wallpaper.transition").at("value") == selected);
  }
  client.call("configuration", "Reset", g_variant_new("(s)", "theme.shell_mode"));
  client.call("configuration", "Reset", g_variant_new("(s)", "backdrop.enabled"));
  client.call("configuration", "Reset", g_variant_new("(s)", "location.auto_locate"));
  client.call("configuration", "Reset", g_variant_new("(s)", "location.sunrise"));
  client.call("configuration", "Reset", g_variant_new("(s)", "weather.unit"));
  client.call("configuration", "Reset", g_variant_new("(s)", "audio.enable_overdrive"));
  client.call("configuration", "Reset", g_variant_new("(s)", "brightness.enable_ddcutil"));
  client.call("configuration", "Reset", g_variant_new("(s)", "control_center.network.wifi_band"));
  client.call("configuration", "Reset", g_variant_new("(s)", "control_center.bluetooth.hide_unnamed"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.time_format"));
  client.call("configuration", "Reset", g_variant_new("(s)", "calendar.enabled"));
  client.call("configuration", "Reset", g_variant_new("(s)", "notification.filter_replace"));
  client.call("configuration", "Reset", g_variant_new("(s)", "tailscale.notify_on_peer_change"));
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.border"));
  client.call("configuration", "Reset", g_variant_new("(s)", "wallpaper.transition"));
  client.call("configuration", "Update", g_variant_new("(sv)", "dock.border_width", g_variant_new_double(2.0)));
  assert(setting(client, "dock.border_width").at("value") == 2.0);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "dock.concave_edge_corners", g_variant_new_boolean(false))
  );
  assert(setting(client, "dock.concave_edge_corners").at("value") == false);
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.border_width"));
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.concave_edge_corners"));
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:border_width", g_variant_new_double(1.5)));
  assert(setting(client, "bar:default:border_width").at("value") == 1.5);
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:font_weight", g_variant_new_int64(700)));
  assert(setting(client, "bar:default:font_weight").at("value") == 700);
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:border_width"));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:font_weight"));
  client.call("configuration", "Update", g_variant_new("(sv)", "theme.niri_layout.gaps", g_variant_new_int64(12)));
  assert(setting(client, "theme.niri_layout.gaps").at("value") == 12);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "theme.niri_layout.overview", g_variant_new_string("backdrop"))
  );
  assert(setting(client, "theme.niri_layout.overview").at("value") == "backdrop");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "theme.niri_layout.overview", g_variant_new_string("cube"))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "theme.niri_layout.gaps"));
  client.call("configuration", "Reset", g_variant_new("(s)", "theme.niri_layout.overview"));
  assert(setting(client, "shell.corner_radius_scale").at("page") == "appearance");
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.corner_radius_scale", g_variant_new_double(1.5)));
  assert(setting(client, "shell.corner_radius_scale").at("value") == 1.5);
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "shell.corner_radius_scale", g_variant_new_double(3))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.corner_radius_scale"));
  assert(setting(client, "shell.lang").at("page") == "system");
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.lang", g_variant_new_string("de")));
  assert(setting(client, "shell.lang").at("value") == "de");
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.lang"));
  assert(setting(client, "shell.lang").at("value") == "");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "shell.lang", g_variant_new_string("not-a-language"))
  ));
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.shadow.direction", g_variant_new_string("up")));
  assert(setting(client, "shell.shadow.direction").at("value") == "up");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.shadow.direction", g_variant_new_string("diagonal"))
  ));
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.panel.glass", g_variant_new_double(0.25)));
  assert(setting(client, "shell.panel.glass").at("value") == 0.25);
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "shell.panel.glass", g_variant_new_double(2)))
  );
  auto templates = setting(client, "theme.templates.builtin_ids");
  assert(templates.at("type") == "template-list");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "theme.templates.builtin_ids", helix::toVariant(templates.at("value")))
  ));
  if (!templates.at("options").empty()) {
    Json selected = Json::array({templates.at("options").at(0)});
    client.call(
        "configuration", "UpdateOrderedList",
        g_variant_new(
            "(s@av@av)", "theme.templates.builtin_ids", helix::toVariant(templates.at("value")),
            helix::toVariant(selected)
        )
    );
    assert(setting(client, "theme.templates.builtin_ids").at("value") == selected);
    assert(rejects(
        client, "configuration", "UpdateOrderedList",
        g_variant_new(
            "(s@av@av)", "theme.templates.builtin_ids", helix::toVariant(selected),
            helix::toVariant(Json::array({"missing-template"}))
        )
    ));
    client.call("configuration", "Reset", g_variant_new("(s)", "theme.templates.builtin_ids"));
  }
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.shadow.direction"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.panel.glass"));

  auto shortcuts = setting(client, "control_center.shortcuts").at("value");
  assert(shortcuts == Json::array({{{"type", "author/plugin:pin"}, {"size", "wide"}}}));
  auto updateShortcuts = [&](const Json& before, const Json& after) {
    return g_variant_new("(s@av@av)", "control_center.shortcuts", helix::toVariant(before), helix::toVariant(after));
  };
  auto pinned = shortcuts;
  pinned.push_back({{"type", "wifi"}, {"size", "large"}});
  client.call("configuration", "UpdateOrderedList", updateShortcuts(shortcuts, pinned));
  assert(setting(client, "control_center.shortcuts").at("value") == pinned);
  assert(rejects(client, "configuration", "UpdateOrderedList", updateShortcuts(shortcuts, Json::array())));
  assert(rejects(
      client, "configuration", "UpdateOrderedList",
      updateShortcuts(pinned, Json::array({{{"type", "unknown"}, {"size", "small"}}}))
  ));
  assert(rejects(
      client, "configuration", "UpdateOrderedList",
      updateShortcuts(pinned, Json::array({{{"type", "wifi"}, {"size", "invalid"}}}))
  ));
  assert(rejects(
      client, "configuration", "UpdateOrderedList",
      updateShortcuts(pinned, Json::array({{{"type", "wifi"}, {"size", "small"}, {"command", "unsupported"}}}))
  ));
  assert(rejects(client, "configuration", "UpdateOrderedList", updateShortcuts(pinned, Json::array({true}))));
  assert(rejects(
      client, "configuration", "UpdateOrderedList",
      updateShortcuts(pinned, Json::array({{{"type", "screen_time"}, {"size", "small"}}}))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "control_center.shortcuts", helix::toVariant(pinned))
  ));
  std::swap(pinned.at(0), pinned.at(1));
  pinned.at(1)["size"] = "large";
  client.call(
      "configuration", "UpdateOrderedList",
      updateShortcuts(setting(client, "control_center.shortcuts").at("value"), pinned)
  );
  assert(setting(client, "control_center.shortcuts").at("value") == pinned);
  client.call("configuration", "UpdateOrderedList", updateShortcuts(pinned, Json::array()));
  assert(setting(client, "control_center.shortcuts").at("value").empty());
  client.call("configuration", "Reset", g_variant_new("(s)", "control_center.shortcuts"));
  assert(setting(client, "control_center.shortcuts").at("value") == shortcuts);
  auto pages = setting(client, "control_center.visible_tabs");
  assert(pages.at("type") == "tab-list");
  assert(std::find(pages.at("options").begin(), pages.at("options").end(), Json("home")) == pages.at("options").end());
  auto visiblePages = pages.at("value");
  assert(std::find(visiblePages.begin(), visiblePages.end(), Json("ai")) == visiblePages.end());
  assert(std::find(visiblePages.begin(), visiblePages.end(), Json("capture")) == visiblePages.end());
  auto updatePages = [&](const Json& before, const Json& after) {
    return g_variant_new("(s@av@av)", "control_center.visible_tabs", helix::toVariant(before), helix::toVariant(after));
  };
  assert(rejects(client, "configuration", "UpdateOrderedList", updatePages(visiblePages, Json::array({"home"}))));
  assert(rejects(
      client, "configuration", "UpdateOrderedList", updatePages(visiblePages, Json::array({"network", "network"}))
  ));
  assert(rejects(client, "configuration", "UpdateOrderedList", updatePages(visiblePages, Json::array({true}))));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "control_center.visible_tabs", helix::toVariant(pages.at("options")))
  ));
  client.call("configuration", "UpdateOrderedList", updatePages(visiblePages, pages.at("options")));
  assert(setting(client, "control_center.visible_tabs").at("value") == pages.at("options"));
  {
    std::ifstream file(root / "state/helix/settings.toml");
    std::string contents((std::istreambuf_iterator<char>(file)), {});
    assert(contents.find("plugin:test-hidden") != std::string::npos);
    assert(
        contents.find("ai-usage") == std::string::npos
        && contents.find("screenshot") == std::string::npos
        && contents.find("screen-record") == std::string::npos
    );
  }
  assert(rejects(client, "configuration", "UpdateOrderedList", updatePages(visiblePages, Json::array())));
  client.call("configuration", "UpdateOrderedList", updatePages(pages.at("options"), Json::array()));
  assert(setting(client, "control_center.visible_tabs").at("value").empty());
  client.call("configuration", "Reset", g_variant_new("(s)", "control_center.visible_tabs"));
  assert(setting(client, "control_center.visible_tabs").at("value") == visiblePages);
  assert(setting(client, "control_center.sidebar").at("options") == Json::array({"full", "compact", "none"}));
  assert(
      setting(client, "shell.panel.control_center_placement").at("options") == Json::array({"attached", "floating"})
  );
  client.call("configuration", "Update", g_variant_new("(sv)", "control_center.sidebar", g_variant_new_string("none")));
  client.call("configuration", "Update", g_variant_new("(sv)", "control_center.width", g_variant_new_int64(900)));
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.panel.control_center_placement", g_variant_new_string("floating"))
  );
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.panel.control_center_position", g_variant_new_string("center_right"))
  );
  assert(setting(client, "control_center.sidebar").at("value") == "none");
  assert(setting(client, "control_center.width").at("value") == 900);
  assert(setting(client, "shell.panel.control_center_placement").at("value") == "floating");
  assert(setting(client, "shell.panel.control_center_position").at("value") == "center_right");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "control_center.sidebar", g_variant_new_string("invalid"))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "control_center.width", g_variant_new_int64(200))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.panel.control_center_position", g_variant_new_string("elsewhere"))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "control_center.sidebar"));
  client.call("configuration", "Reset", g_variant_new("(s)", "control_center.width"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.panel.control_center_placement"));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.panel.control_center_position"));
  assert(setting(client, "control_center.sidebar").at("value") == "compact");
  assert(setting(client, "control_center.width").at("value") == 700);
  assert(rejects(client, "appearance", "SearchWallhaven", g_variant_new("(@a{sv})", helix::toVariant({{"page", 0}}))));
  assert(rejects(client, "appearance", "SearchWallhaven", g_variant_new("(@a{sv})", helix::toVariant({{"sort", 7}}))));
  assert(
      rejects(client, "appearance", "SearchWallhaven", g_variant_new("(@a{sv})", helix::toVariant({{"width", 1920}})))
  );
  assert(
      rejects(client, "appearance", "SearchWallhaven", g_variant_new("(@a{sv})", helix::toVariant({{"page", 1.5}})))
  );
  assert(
      rejects(client, "appearance", "SearchWallhaven", g_variant_new("(@a{sv})", helix::toVariant({{"nsfw", true}})))
  );
  assert(rejects(client, "appearance", "GetWallhaven", g_variant_new("(t)", std::uint64_t(999))));
  assert(rejects(client, "appearance", "DownloadWallhaven", g_variant_new("(ts)", std::uint64_t(999), "abc123")));
  assert(rejects(client, "appearance", "SetWallhavenApiKey", g_variant_new("(s)", "invalid\nkey")));
  client.call("appearance", "SetWallhavenApiKey", g_variant_new("(s)", "TestWallhavenKey987"));
  assert(client.call("appearance", "GetSnapshot").at("wallhaven-key-available") == true);
  assert(client.call("appearance", "GetSnapshot").dump().find("TestWallhavenKey987") == std::string::npos);
  assert(client.call("configuration", "GetSettings").dump().find("TestWallhavenKey987") == std::string::npos);
  const auto previewFile = root / "preview.ppm";
  std::ofstream(previewFile) << "P3\n1 1\n255\n80 100 200\n";
  client.call("appearance", "SetWallpaper", g_variant_new("(s)", previewFile.c_str()));
  assert(client.call("appearance", "GetSnapshot").at("wallpaper") == previewFile.string());

  for (const auto& item : client.call("configuration", "GetSettings")) {
    const auto type = item.at("type").get<std::string>();
    if (type == "boolean")
      assert(item.at("value").is_boolean());
    else if (type == "integer")
      assert(item.at("value").is_number_integer());
    else if (type == "number")
      assert(item.at("value").is_number());
    else if (
        type == "application-list"
        || type == "widget-list"
        || type == "display-list"
        || type == "notification-rules"
        || type == "tab-list"
        || type == "shortcut-list"
        || type == "key-chords"
        || type == "template-list"
        || type == "transition-list"
    )
      assert(item.at("value").is_array());
    else
      assert(item.at("value").is_string());
  }
  assert(setting(client, "dock.icon_size").at("value").is_number_integer());
  assert(setting(client, "audio.sound_volume").at("value").is_number());
  assert(setting(client, "idle:lock:timeout").at("page") == "power");
  client.call("configuration", "Update", g_variant_new("(sv)", "idle:lock:timeout", g_variant_new_double(420)));
  assert(setting(client, "idle:lock:timeout").at("value") == 420);
  assert(setting(client, "idle:lock:action").at("value") == "lock");
  assert(setting(client, "idle:screen-off:action").at("value") == "screen_off");
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "idle:lock:timeout", g_variant_new_double(-1)))
  );
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "idle:lock:action", g_variant_new_string("unknown"))
  ));
  client.call("configuration", "Update", g_variant_new("(sv)", "idle:lock:locked_timeout", g_variant_new_double(0)));
  client.call("configuration", "Update", g_variant_new("(sv)", "battery.warning_threshold", g_variant_new_int64(25)));
  assert(setting(client, "battery.warning_threshold").at("value") == 25);
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "battery.warning_threshold", g_variant_new_int64(101))
  ));

  assert(setting(client, "shell.offline_mode").at("page") == "privacy-security");
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.offline_mode", g_variant_new_boolean(true)));
  assert(setting(client, "shell.offline_mode").at("value") == true);
  assert(!client.call("appearance", "GetSnapshot").at("wallhaven-online").get<bool>());
  assert(rejects(client, "appearance", "SearchWallhaven", g_variant_new("(@a{sv})", helix::toVariant(Json::object()))));
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.password_style", g_variant_new_string("random")));
  assert(setting(client, "shell.password_style").at("value") == "random");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "shell.password_style", g_variant_new_string("invalid"))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "lockscreen.blur_intensity", g_variant_new_double(1.1))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "shell.telemetry_enabled", g_variant_new_string("yes"))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.offline_mode"));

  client.call(
      "configuration", "Update", g_variant_new("(sv)", "hot_corners.top_left.action", g_variant_new_string("launcher"))
  );
  assert(setting(client, "hot_corners.top_left.action").at("value") == "launcher");
  client.call("configuration", "Update", g_variant_new("(sv)", "hot_corners.delay_ms", g_variant_new_int64(300)));
  assert(setting(client, "hot_corners.delay_ms").at("value") == 300);
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "hot_corners.delay_ms", g_variant_new_int64(2001))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "hot_corners.top_left.action", g_variant_new_string("invalid"))
  ));
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "hot_corners.top_left.command", g_variant_new_string("existing-command"))
  );
  client.call("configuration", "Reset", g_variant_new("(s)", "hot_corners.top_left.action"));
  assert(setting(client, "hot_corners.top_left.command").at("value") == "existing-command");

  assert(setting(client, "shell.screen_time_enabled").at("value") == false);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.screen_time_enabled", g_variant_new_boolean(true))
  );
  assert(setting(client, "shell.screen_time_enabled").at("value") == true);
  client.call("configuration", "Reset", g_variant_new("(s)", "shell.screen_time_enabled"));
  assert(setting(client, "shell.screen_time_enabled").at("value") == false);

  const auto copyKeys = setting(client, "keybinds.copy").at("value");
  assert(setting(client, "keybinds.copy").at("page") == "keyboard");
  auto keyUpdate = [&](const Json& before, const Json& after) {
    return g_variant_new("(s@av@av)", "keybinds.copy", helix::toVariant(before), helix::toVariant(after));
  };
  const auto editedKeys = Json::array({"Alt+F9", "Ctrl+F8"});
  client.call("configuration", "UpdateOrderedList", keyUpdate(copyKeys, editedKeys));
  assert(setting(client, "keybinds.copy").at("value") == editedKeys);
  assert(rejects(client, "configuration", "UpdateOrderedList", keyUpdate(copyKeys, Json::array({"Ctrl+c"}))));
  for (const auto& invalid :
       {Json::array(), Json::array({"not-a-key"}), Json::array({"Super+c"}), Json::array({"Ctrl+c", "Ctrl+c"}),
        Json::array({42})})
    assert(rejects(client, "configuration", "UpdateOrderedList", keyUpdate(editedKeys, invalid)));
  assert(
      rejects(client, "configuration", "Update", g_variant_new("(sv)", "keybinds.copy", helix::toVariant(editedKeys)))
  );
  client.call("configuration", "Reset", g_variant_new("(s)", "keybinds.copy"));
  assert(setting(client, "keybinds.copy").at("value") == copyKeys);

  // Clipboard policy is desktop-only configuration, never a clipboard-content snapshot.
  assert(setting(client, "battery.warning_threshold").at("page") == "power");
  auto clipboardPolicy = setting(client, "shell.clipboard_enabled");
  assert(clipboardPolicy.at("page") == "privacy-security" && clipboardPolicy.at("group") == "Clipboard");
  const auto retained = clipboardPolicy.at("value");
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.clipboard_enabled", g_variant_new_boolean(false))
  );
  assert(setting(client, "shell.clipboard_enabled").at("value") == false);
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.clipboard_keep_from_closed_apps", g_variant_new_boolean(false))
  );
  assert(setting(client, "shell.clipboard_keep_from_closed_apps").at("value") == false);
  assert(setting(client, "shell.clipboard_enabled").at("value") == false);
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.clipboard_history_max_entries", g_variant_new_int64(250))
  );
  assert(setting(client, "shell.clipboard_history_max_entries").at("value") == 250);
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.clipboard_history_max_entries", g_variant_new_int64(9))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.clipboard_history_max_entries", g_variant_new_int64(10001))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "shell.clipboard_enabled", g_variant_new_string("yes"))
  ));
  const auto paste = setting(client, "shell.clipboard_auto_paste").at("value");
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "shell.clipboard_auto_paste", g_variant_new_string("off"))
  );
  assert(setting(client, "shell.clipboard_auto_paste").at("value") == "off");
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "shell.clipboard_auto_paste", g_variant_new_string("invalid"))
  ));
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.clipboard_confirm_clear_history", g_variant_new_boolean(false))
  );
  assert(setting(client, "shell.clipboard_confirm_clear_history").at("value") == false);
  client.call(
      "configuration", "Update",
      g_variant_new("(sv)", "shell.clipboard_image_action_command", g_variant_new_string("existing-editor {path}"))
  );
  assert(setting(client, "shell.clipboard_image_action_command").at("value") == "existing-editor {path}");
  for (const auto* id :
       {"shell.clipboard_enabled", "shell.clipboard_keep_from_closed_apps", "shell.clipboard_history_max_entries",
        "shell.clipboard_confirm_clear_history", "shell.clipboard_auto_paste", "shell.clipboard_image_action_command"})
    client.call("configuration", "Reset", g_variant_new("(s)", id));
  assert(setting(client, "shell.clipboard_enabled").at("value") == retained);
  assert(setting(client, "shell.clipboard_auto_paste").at("value") == paste);

  NotificationFilterConfig firstRule;
  firstRule.name = "test-first";
  firstRule.match = "test-app";
  firstRule.matchContent = "urgent";
  firstRule.showToast = false;
  firstRule.overrideDuration = 1234;
  firstRule.allowedUrgencies = {"critical"};
  auto rule = helix::notificationRule(firstRule);
  auto rules = setting(client, "notification.filter").at("value");
  auto updateRules = [&](const Json& before, const Json& after) {
    return g_variant_new("(s@av@av)", "notification.filter", helix::toVariant(before), helix::toVariant(after));
  };
  client.call("configuration", "UpdateOrderedList", updateRules(rules, Json::array({rule})));
  assert(setting(client, "notification.filter").at("value") == Json::array({rule}));
  assert(rejects(client, "configuration", "UpdateOrderedList", updateRules(rules, Json::array())));
  rules = setting(client, "notification.filter").at("value");
  assert(rejects(client, "configuration", "UpdateOrderedList", updateRules(rules, Json::array({rule, rule}))));
  auto invalidRule = rule;
  invalidRule["match_content"] = "[";
  assert(rejects(client, "configuration", "UpdateOrderedList", updateRules(rules, Json::array({invalidRule}))));
  invalidRule = rule;
  invalidRule["play_sound"] = "true";
  assert(rejects(client, "configuration", "UpdateOrderedList", updateRules(rules, Json::array({invalidRule}))));
  invalidRule = rule;
  invalidRule["allowed_urgencies"] = Json::array({"emergency"});
  assert(rejects(client, "configuration", "UpdateOrderedList", updateRules(rules, Json::array({invalidRule}))));
  invalidRule = rule;
  invalidRule["override_duration"] = 2147483648LL;
  assert(rejects(client, "configuration", "UpdateOrderedList", updateRules(rules, Json::array({invalidRule}))));
  invalidRule = rule;
  invalidRule["match"] = "";
  invalidRule["match_content"] = "";
  assert(rejects(client, "configuration", "UpdateOrderedList", updateRules(rules, Json::array({invalidRule}))));
  auto secondRule = rule;
  secondRule["name"] = "test-second";
  secondRule["match"] = "other-app";
  client.call("configuration", "UpdateOrderedList", updateRules(rules, Json::array({secondRule, rule})));

  auto displays = setting(client, "notification.monitors").at("value");
  auto updateDisplays = [&](const Json& before, const Json& after) {
    return g_variant_new("(s@av@av)", "notification.monitors", helix::toVariant(before), helix::toVariant(after));
  };
  client.call("configuration", "UpdateOrderedList", updateDisplays(displays, Json::array({"DP-1", "eDP-1"})));
  assert(setting(client, "notification.monitors").at("value") == Json::array({"DP-1", "eDP-1"}));
  assert(
      rejects(client, "configuration", "UpdateOrderedList", updateDisplays(displays, Json::array({"stale-display"})))
  );
  displays = setting(client, "notification.monitors").at("value");
  assert(
      rejects(client, "configuration", "UpdateOrderedList", updateDisplays(displays, Json::array({"DP-1", "DP-1"})))
  );
  assert(rejects(client, "configuration", "UpdateOrderedList", updateDisplays(displays, Json::array({42}))));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "notification.background_opacity", g_variant_new_double(1.2))
  ));
  client.call("configuration", "Update", g_variant_new("(sv)", "osd.orientation", g_variant_new_string("vertical")));
  assert(setting(client, "osd.orientation").at("value") == "vertical");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "osd.orientation", g_variant_new_string("diagonal"))
  ));

  assert(setting(client, "accessibility.high_contrast").at("page") == "accessibility");
  assert(setting(client, "notification.position").at("page") == "notifications");
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "notification.position", g_variant_new_string("bottom_center"))
  );
  assert(setting(client, "notification.position").at("value") == "bottom_center");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "notification.position", g_variant_new_string("middle"))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "notification.max_visible", g_variant_new_int64(21))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "notification.keep_dismissed_in_history", g_variant_new_string("true"))
  ));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "notification.history_retention_hours", g_variant_new_int64(48))
  );

  auto widgets = setting(client, "bar:default:center").at("value");
  auto updateWidgets = [&](const Json& before, const Json& after) {
    return g_variant_new("(s@av@av)", "bar:default:center", helix::toVariant(before), helix::toVariant(after));
  };
  client.call("configuration", "UpdateOrderedList", updateWidgets(widgets, Json::array({"output_volume", "clock"})));
  assert(setting(client, "bar:default:center").at("value") == Json::array({"output_volume", "clock"}));
  assert(rejects(client, "configuration", "UpdateOrderedList", updateWidgets(widgets, Json::array({"clock"}))));
  widgets = setting(client, "bar:default:center").at("value");
  assert(
      rejects(client, "configuration", "UpdateOrderedList", updateWidgets(widgets, Json::array({"missing-widget"})))
  );
  assert(
      rejects(client, "configuration", "UpdateOrderedList", updateWidgets(widgets, Json::array({"clock", "clock"})))
  );
  assert(rejects(
      client, "configuration", "UpdateOrderedList",
      g_variant_new("(s@av@av)", "dock.enabled", helix::toVariant(Json::array()), helix::toVariant(Json::array()))
  ));

  auto favorites = setting(client, "dock.pinned").at("value");
  auto updateFavorites = [&](const Json& before, const Json& after) {
    return g_variant_new("(s@av@av)", "dock.pinned", helix::toVariant(before), helix::toVariant(after));
  };
  client.call(
      "configuration", "UpdateApplicationList", updateFavorites(favorites, Json::array({"test-first", "test-second"}))
  );
  assert(setting(client, "dock.pinned").at("value") == Json::array({"test-first", "test-second"}));
  assert(rejects(client, "configuration", "UpdateApplicationList", updateFavorites(favorites, Json::array({"stale"}))));
  favorites = setting(client, "dock.pinned").at("value");
  assert(rejects(
      client, "configuration", "UpdateApplicationList",
      updateFavorites(favorites, Json::array({"duplicate", "duplicate"}))
  ));
  assert(rejects(client, "configuration", "UpdateApplicationList", updateFavorites(favorites, Json::array({true}))));
  assert(rejects(
      client, "configuration", "UpdateApplicationList", updateFavorites(favorites, Json::array({"/invalid/path"}))
  ));
  client.call(
      "configuration", "UpdateApplicationList", updateFavorites(favorites, Json::array({"test-second", "test-first"}))
  );
  assert(setting(client, "dock.pinned").at("value") == Json::array({"test-second", "test-first"}));

  client.call(
      "configuration", "Update", g_variant_new("(sv)", "dock.launcher_position", g_variant_new_string("start"))
  );
  assert(setting(client, "dock.launcher_position").at("value") == "start");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "dock.launcher_position", g_variant_new_string("middle"))
  ));
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "dock.magnification_scale", g_variant_new_double(3))
  ));
  client.call("configuration", "Update", g_variant_new("(sv)", "shell.launcher.app_grid", g_variant_new_boolean(true)));
  assert(setting(client, "shell.launcher.app_grid").at("value") == true);
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:thickness", g_variant_new_int64(52)));
  assert(setting(client, "bar:default:thickness").at("value") == 52);
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "bar:default:thickness", g_variant_new_int64(301))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "bar:default:reserve_space", g_variant_new_string("true"))
  ));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "bar:default:reserve_space", g_variant_new_boolean(false))
  );
  assert(setting(client, "bar:default:reserve_space").at("value") == false);
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:reserve_space"));
  assert(!setting(client, "bar:default:reserve_space").at("overridden").get<bool>());

  assert(setting(client, "wallpaper.automation.interval_seconds").at("value").is_number_integer());
  client.call("configuration", "Update", g_variant_new("(sv)", "wallpaper.fill_mode", g_variant_new_string("fit")));
  assert(setting(client, "wallpaper.fill_mode").at("value") == "fit");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "wallpaper.fill_mode", g_variant_new_string("invalid"))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "wallpaper.automation.interval_seconds", g_variant_new_int64(0))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "wallpaper.automation.interval_seconds", g_variant_new_double(60.5))
  ));
  client.call(
      "configuration", "Update", g_variant_new("(sv)", "wallpaper.directory", g_variant_new_string(root.c_str()))
  );
  assert(setting(client, "wallpaper.directory").at("value") == root.string());
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "wallpaper.directory", g_variant_new_string("relative-folder"))
  ));
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "wallpaper.directory", g_variant_new_string(source.c_str()))
  ));
  client.call("configuration", "Reset", g_variant_new("(s)", "wallpaper.directory"));
  assert(!setting(client, "wallpaper.directory").at("overridden").get<bool>());

  client.call("configuration", "Update", g_variant_new("(sv)", "dock.enabled", g_variant_new_boolean(true)));
  assert(setting(client, "dock.enabled").at("value") == true);
  client.call("configuration", "Update", g_variant_new("(sv)", "bar:default:position", g_variant_new_string("bottom")));
  assert(setting(client, "bar:default:position").at("value") == "bottom");
  assert(rejects(
      client, "configuration", "Update", g_variant_new("(sv)", "bar:default:position", g_variant_new_string("invalid"))
  ));
  assert(rejects(client, "configuration", "CommitOverrides", g_variant_new("(ss)", "", "[dock]\nenabled=false\n")));
  assert(setting(client, "dock.enabled").at("value") == true);
  {
    std::ofstream file(source);
    file << "[theme]\nmode = \"light\"\n";
  }
  for (unsigned i = 0; i < 50 && setting(client, "theme.mode").at("value") != "light"; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  assert(setting(client, "theme.mode").at("value") == "light");
  service.stop();
  // Availability comes from a bus name watch, which reports once the main loop has run (the shell and the apps always
  // run one).
  for (unsigned i = 0; i < 100 && client.available(); ++i) {
    while (g_main_context_pending(nullptr))
      g_main_context_iteration(nullptr, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(!client.available());
  service.start(argv[1]);
  waitForService(client);
  assert(!client.call("configuration", "GetSnapshot").at("helix-desktop").get<bool>());
  assert(setting(client, "theme.mode").at("value") == "light");
  client.call("configuration", "RegisterDesktop");
  assert(setting(client, "dock.enabled").at("value") == true);
  assert(setting(client, "wallpaper.fill_mode").at("value") == "fit");
  assert(setting(client, "bar:default:thickness").at("value") == 52);
  assert(setting(client, "dock.launcher_position").at("value") == "start");
  assert(setting(client, "dock.pinned").at("value") == Json::array({"test-second", "test-first"}));
  client.call("configuration", "Reset", g_variant_new("(s)", "dock.pinned"));
  assert(!setting(client, "dock.pinned").at("overridden").get<bool>());
  assert(setting(client, "bar:default:center").at("value") == Json::array({"output_volume", "clock"}));
  client.call("configuration", "Reset", g_variant_new("(s)", "bar:default:center"));
  assert(!setting(client, "bar:default:center").at("overridden").get<bool>());
  assert(setting(client, "notification.position").at("value") == "bottom_center");
  assert(setting(client, "notification.history_retention_hours").at("value") == 48);
  client.call("configuration", "Reset", g_variant_new("(s)", "notification.position"));
  assert(!setting(client, "notification.position").at("overridden").get<bool>());
  assert(setting(client, "notification.monitors").at("value") == Json::array({"DP-1", "eDP-1"}));
  assert(setting(client, "osd.orientation").at("value") == "vertical");
  client.call("configuration", "Reset", g_variant_new("(s)", "notification.monitors"));
  assert(!setting(client, "notification.monitors").at("overridden").get<bool>());
  assert(setting(client, "notification.filter").at("value") == Json::array({secondRule, rule}));
  client.call("configuration", "UpdateOrderedList", updateRules(Json::array({secondRule, rule}), Json::array()));
  assert(setting(client, "notification.filter").at("value").empty());
  client.call("configuration", "Reset", g_variant_new("(s)", "notification.filter"));
  {
    std::ofstream file(source);
    file << "[theme]\nmode = \"light\"\n[notification.filter.base]\nmatch = \"base-app\"\n";
  }
  // Force a configuration read through an unrelated validated mutation.
  client.call("configuration", "Update", g_variant_new("(sv)", "audio.enable_sounds", g_variant_new_boolean(false)));
  auto baseRules = setting(client, "notification.filter").at("value");
  assert(baseRules.size() == 1 && baseRules.at(0).at("name") == "base");
  client.call("configuration", "UpdateOrderedList", updateRules(baseRules, Json::array()));
  assert(setting(client, "notification.filter").at("value").empty());
  client.call("configuration", "Reset", g_variant_new("(s)", "notification.filter"));
  assert(setting(client, "notification.filter").at("value").size() == 1);

  assert(!setting(client, "notification.filter").at("overridden").get<bool>());
  assert(setting(client, "idle:lock:timeout").at("value") == 420);
  assert(setting(client, "battery.warning_threshold").at("value") == 25);
  client.call("configuration", "Reset", g_variant_new("(s)", "idle.behavior"));
  assert(!setting(client, "idle:lock:timeout").at("group-overridden").get<bool>());
  {
    std::ofstream file(source, std::ios::app);
    file << "\n[idle.behavior.custom]\nenabled = false\ntimeout = 90\naction = \"command\"\ncommand = "
            "\"existing-command\"\nresume_command = \"existing-resume\"\n";
  }
  client.call("configuration", "Update", g_variant_new("(sv)", "idle:custom:timeout", g_variant_new_double(120)));
  assert(setting(client, "idle:custom:action").at("value") == "command");
  {
    std::ifstream file(root / "state/helix/settings.toml");
    std::string content((std::istreambuf_iterator<char>(file)), {});
    assert(
        content.find("existing-command") != std::string::npos && content.find("existing-resume") != std::string::npos
    );
  }
  client.call("configuration", "Reset", g_variant_new("(s)", "idle.behavior"));
  assert(setting(client, "idle:custom:timeout").at("value") == 90);
  const auto savedPalette = client.call("appearance", "SavePalette", g_variant_new("(s)", "current"));
  assert(savedPalette.is_string() && !savedPalette.get<std::string>().empty());
  assert(setting(client, "theme.source").at("value") == "custom");
  const auto custom = setting(client, "theme.custom_palette");
  assert(custom.at("value") == savedPalette);
  assert(std::ranges::find(custom.at("options"), savedPalette) != custom.at("options").end());
  assert(rejects(
      client, "configuration", "Update",
      g_variant_new("(sv)", "theme.custom_palette", g_variant_new_string("missing-palette"))
  ));
  service.stop();
  std::filesystem::remove_all(root);
  return 0;
}
