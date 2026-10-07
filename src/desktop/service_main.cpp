#include "desktop/dns_update.h"
#include "desktop/display_inventory.h"
#include "desktop/wallhaven.h"
#include "theme/community_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/builtin_palettes.h"
#include <pipewire/pipewire.h>
#include <cerrno>
#include "desktop/input_meter.h"
#include "desktop_api/protocol.h"
#include "desktop/settings_catalog.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "dbus/system_bus.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "net/http_client.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/wireplumber_mixer.h"
#include "theme/theme_service.h"
#include "theme/template_apply_service.h"
#include "theme/json_output.h"
#include "theme/palette.h"
#include <filesystem>
#include <sys/utsname.h>
#include <glib-unix.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>

#include "desktop/agent_endpoint.h"
#include "helix_desktop_introspection.h"

namespace helix {
namespace {
const char* introspection = helix::protocols::desktopIntrospectionXml;
Json audioNodes(const std::vector<AudioNode>& nodes) {
  Json output = Json::array();
  for (const auto& node : nodes) output.push_back({{"id", node.id}, {"name", node.name},
    {"description", audioDeviceLabel(node)}, {"volume", node.volume}, {"muted", node.muted},
    {"default", node.isDefault}, {"available", node.available}});
  return output;
}
Json palettePreviews() {
  static const Json builtins = [] {
    Json result = Json::array();
    for (const auto& palette : noctalia::theme::builtinPalettes()) {
      const auto colors = noctalia::theme::expandBuiltinPalette(palette);
      auto mode = [](const auto& tokens) {
        Json accents = Json::array();
        for (const char* role : {"primary", "secondary", "tertiary"})
          if (const auto color = tokens.find(role); color != tokens.end()) accents.push_back(color->second & 0xffffff);
        const auto surface = tokens.find("surface");
        return Json{{"surface", surface == tokens.end() ? 0u : surface->second & 0xffffff}, {"accents", accents}};
      };
      result.push_back({{"source", "builtin"}, {"name", palette.name}, {"dark", mode(colors.dark)}, {"light", mode(colors.light)}});
    }
    return result;
  }();
  Json result = builtins;
  auto color = [](const std::string& text) -> std::optional<unsigned> {
    if (text.size() != 7 || text[0] != '#') return std::nullopt;
    unsigned value = 0;
    for (std::size_t i = 1; i < 7; ++i) {
      const int digit = g_ascii_xdigit_value(text[i]); if (digit < 0) return std::nullopt;
      value = (value << 4) | unsigned(digit);
    }
    return value;
  };
  auto add = [&](const auto& palette, const char* source) {
    auto mode = [&](const auto& preview) {
      Json accents = Json::array();
      for (const auto& text : preview.accents) if (auto value = color(text)) accents.push_back(*value);
      return Json{{"surface", color(preview.surface).value_or(0u)}, {"accents", accents}};
    };
    result.push_back({{"source", source}, {"name", palette.name}, {"dark", mode(palette.preview.dark)}, {"light", mode(palette.preview.light)}});
  };
  unsigned shown = 0;
  for (const auto& palette : noctalia::theme::availableCommunityPalettes()) { if (++shown > 100) break; add(palette, "community"); }
  shown = 0;
  for (const auto& palette : noctalia::theme::availableCustomPalettes()) { if (++shown > 100) break; add(palette, "custom"); }
  return result;
}
struct Operation {
  std::uint64_t id;
  std::string interface;
  std::string sender;
  std::string action;
  std::function<bool()> done;
  std::function<void()> cancel;
  std::chrono::steady_clock::time_point deadline;
};
class Service {
public:
  ConfigService config;
  HttpClient http;
  WallhavenService wallhaven{config, http, [this] { return desktop(); }};
  noctalia::theme::CommunityPaletteService paletteCatalog{http};
  noctalia::theme::ThemeService theme{config, http};
  noctalia::theme::TemplateApplyService templates{config};
  std::unique_ptr<SystemBus> system;
  std::unique_ptr<NetworkManagerService> network;
  std::unique_ptr<NetworkSecretAgent> secrets;
  std::unique_ptr<BluetoothService> bluetooth;
  std::unique_ptr<BluetoothAgent> pairing;
  std::unique_ptr<PipeWireService> audio;
  std::unique_ptr<WirePlumberMixer> mixer;
  spa_hook audioHealthListener{};
  bool audioHealthListening = false;
  bool audioBroken = false;
  struct Meter { std::uint32_t source; std::unique_ptr<InputMeter> capture; std::chrono::steady_clock::time_point expires; };
  std::map<std::string, Meter> meters;
  std::chrono::steady_clock::time_point nextMeterUpdate{};
  GDBusConnection* bus = nullptr;
  std::string desktopOwner;
  Json desktopCapabilities = Json::object();
  std::string networkOwner;
  std::string bluetoothOwner;
  std::vector<Operation> operations;
  std::unique_ptr<agent::AgentEndpoint> agentEndpoint;
  std::uint64_t nextOperation = 1;
  std::shared_ptr<DisplayInventory> displays = std::make_shared<DisplayInventory>();
  Json appearance = {{"available", false}};
  GMainLoop* loop = nullptr;
  guint ownerWatch = 0;
  std::vector<guint> registrations;
  GDBusNodeInfo* info = nullptr;
  guint source = 0;
  // The tick polls PipeWire, the mixer, HTTP and deferred calls by hand. It runs fast while anything is happening and slowly
  // when idle: a permanent 16 ms timer cost about 2.5% of a core with nothing to do. Anything that needs a quick reaction
  // (a D-Bus call, a ready fd, running operations, meters, transfers) calls wake().
  static constexpr guint kBusyIntervalMs = 16, kIdleIntervalMs = 100;
  static constexpr auto kBusyHold = std::chrono::seconds(1);
  guint tickIntervalMs = kBusyIntervalMs;
  std::chrono::steady_clock::time_point busyUntil{};
  std::string networkBackendOwner, bluetoothBackendOwner;
  std::chrono::steady_clock::time_point nextBackendCheck{};
  void failOperations(const char* interface) {
    for (const auto& operation : operations) if (operation.interface == interface)
      emit(interface, "Completed", g_variant_new("(tbs)", operation.id, false, "Device service disconnected"), operation.sender);
    std::erase_if(operations, [interface](const auto& operation) { return operation.interface == interface; });
  }
  std::string backendOwner(const char* name) {
    if (!system) return {};
    try {
      auto proxy = sdbus::createProxy(system->connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
      std::string owner;
      proxy->callMethod("GetNameOwner").onInterface("org.freedesktop.DBus").withArguments(std::string(name)).storeResultsTo(owner);
      return owner;
    } catch (...) { return {}; }
  }
  void recoverBackends() {
    if (!system) try { system = std::make_unique<SystemBus>(); } catch (...) {}
    const auto nmOwner = backendOwner("org.freedesktop.NetworkManager");
    const auto bluezOwner = backendOwner("org.bluez");
    if (nmOwner != networkBackendOwner) {
      failOperations(networkInterface); networkOwner.clear(); secrets.reset(); network.reset();
      networkBackendOwner = nmOwner; changed(networkInterface); changed(configurationInterface);
    }
    if (bluezOwner != bluetoothBackendOwner) {
      failOperations(bluetoothInterface); bluetoothOwner.clear(); pairing.reset(); bluetooth.reset();
      bluetoothBackendOwner = bluezOwner; changed(bluetoothInterface); changed(configurationInterface);
    }


    try {
      if (!nmOwner.empty() && !network) {
        network = std::make_unique<NetworkManagerService>(*system);
        network->setChangeCallback([this](const NetworkState&, NetworkChangeOrigin) { changed(networkInterface); });
        secrets = std::make_unique<NetworkSecretAgent>(*system);
        changed(configurationInterface); changed(networkInterface);
        secrets->setRequestCallback([this](const NetworkSecretAgent::SecretRequest& request) {
          if (networkOwner.empty()) { secrets->cancelSecret(); return; }
          emit(networkInterface, "Prompt", g_variant_new("(@a{sv})", toVariant({{"ssid", request.ssid}, {"setting", request.settingName}})), networkOwner);
        });
      }
    } catch (...) { secrets.reset(); network.reset(); }
    try {
      if (!bluezOwner.empty() && !bluetooth) {
        bluetooth = std::make_unique<BluetoothService>(*system, nullptr);
        bluetooth->setStateCallback([this](const BluetoothState&, BluetoothStateChangeOrigin) { changed(bluetoothInterface); });
        bluetooth->setDevicesCallback([this](const auto&) { changed(bluetoothInterface); });
        pairing = std::make_unique<BluetoothAgent>(*system);
        changed(configurationInterface); changed(bluetoothInterface);
        pairing->setRequestCallback([this](const BluetoothPairingRequest& request) {
          if (bluetoothOwner.empty()) { pairing->rejectConfirm(); return; }
          emit(bluetoothInterface, "Prompt", g_variant_new("(@a{sv})", toVariant({{"kind", int(request.kind)},
            {"device", request.devicePath}, {"passkey", request.passkey}, {"pin", request.pin}, {"entered", request.entered}})), bluetoothOwner);
        });
      }
    } catch (...) { pairing.reset(); bluetooth.reset(); }
    if (audioBroken) {
      failOperations(audioInterface); meters.clear();
      if (audioHealthListening) { spa_hook_remove(&audioHealthListener); audioHealthListening = false; }
      audio.reset(); mixer.reset(); audioBroken = false;
      changed(audioInterface); changed(configurationInterface);
    }
    if (!audio) try {
      audio = std::make_unique<PipeWireService>();
      static const pw_core_events health = {.version = PW_VERSION_CORE_EVENTS,
        .error = +[](void* data, std::uint32_t id, int, int error, const char*) {
          auto& self = *static_cast<Service*>(data);
          if (id == PW_ID_CORE && (error == -EPIPE || error == -ECONNRESET || error == -ENOTCONN)) {
            self.audioBroken = true; self.nextBackendCheck = {};
          }
        }};
      pw_core_add_listener(audio->coreHandle(), &audioHealthListener, &health, this); audioHealthListening = true;
      mixer = std::make_unique<WirePlumberMixer>();
      audio->setWirePlumberMixer(mixer.get());
      mixer->setChangeCallback([this](std::uint32_t id, float volume, bool muted) { audio->onMixerVolumeChanged(id, volume, muted); });
      audio->setChangeCallback([this] { changed(audioInterface); });
      changed(configurationInterface); changed(audioInterface);
    } catch (const std::exception&) { if (audioHealthListening) { spa_hook_remove(&audioHealthListener); audioHealthListening = false; } audio.reset(); mixer.reset(); g_debug("Audio backend unavailable"); }
  }
  void initialize() {
    recoverBackends();
    paletteCatalog.setReadyCallback([this] { changed(configurationInterface); });
    templates.setAfterApplyCallback([this](std::string_view mode, bool changed) {
      if (!desktopOwner.empty()) {
        auto* schemaSource = g_settings_schema_source_get_default();
        auto* schema = schemaSource ? g_settings_schema_source_lookup(schemaSource, "org.gnome.desktop.interface", true) : nullptr;
        if (schema) {
          auto* preferences = g_settings_new_full(schema, nullptr, nullptr);
          g_settings_set_string(preferences, "color-scheme", mode == "light" ? "prefer-light" : "prefer-dark");
          g_object_unref(preferences); g_settings_schema_unref(schema);
        }
        schema = schemaSource ? g_settings_schema_source_lookup(schemaSource, "org.gnome.desktop.a11y.interface", true) : nullptr;
        if (schema) {
          auto* preferences = g_settings_new_full(schema, nullptr, nullptr);
          g_settings_set_boolean(preferences, "high-contrast", config.config().accessibility.highContrast);
          g_object_unref(preferences); g_settings_schema_unref(schema);
        }
      }
      emit(appearanceInterface, "Applied", g_variant_new("(sb)", std::string(mode).c_str(), changed));
    });
    theme.setResolvedCallback([this](const noctalia::theme::GeneratedPalette& palette, std::string_view mode) {
      const bool paletteChanged = appearance.value("mode", std::string{}) != mode ||
        !appearance.contains("dark") || appearance.at("dark") != Json(palette.dark) || appearance.at("light") != Json(palette.light);
      appearance = {{"available", true}, {"mode", mode}, {"shell-mode", theme.resolvedShellMode()},
        {"dark", palette.dark}, {"light", palette.light}, {"high-contrast", config.config().accessibility.highContrast},
        {"animations", config.config().shell.animation.enabled}, {"helix-desktop", desktop()},
        {"font-family", config.config().shell.fontFamily}};
      // Only Helix sessions may export into desktop-wide appearance files.
      if (!desktopOwner.empty()) {
        templates.apply(palette, mode, false, paletteChanged);

      }
      changed(appearanceInterface);
    });
    config.addReloadCallback([this] { theme.apply(); changed(configurationInterface); });
    config.setWallpaperChangeCallback([this] { theme.apply(); });
    theme.apply();
  }
  ~Service() {
    displays->shutdown();
    meters.clear();
    if (audioHealthListening) spa_hook_remove(&audioHealthListener);
    if (source) g_source_remove(source);
    if (ownerWatch) g_dbus_connection_signal_unsubscribe(bus, ownerWatch);
    for (auto id : registrations) g_dbus_connection_unregister_object(bus, id);
    if (info) g_dbus_node_info_unref(info);
    if (bus) g_object_unref(bus);
    if (loop) g_main_loop_unref(loop);
  }
  void emit(const char* interface, const char* signal, GVariant* value = nullptr, const std::string& destination = {}) {
    if (bus) g_dbus_connection_emit_signal(bus, destination.empty() ? nullptr : destination.c_str(), objectPath,
        interface, signal, value, nullptr);
  }
  void changed(const char* interface) {
    // Network, Bluetooth and Audio state changes constantly on a real machine (signal strength, devices) and the settings catalog
    // does not show any of it, so they must not throw the cache away.
    const std::string_view name = interface;
    if (name != networkInterface && name != bluetoothInterface && name != audioInterface) ++catalogEpoch;
    emit(interface, "Changed");
  }
  // GetSettings builds a catalog of several hundred entries, and the Settings app asks for it every few seconds. Rebuilding it
  // each time cost about 2.5% of a core with nothing changing. It is reused until something notifies a change (every config
  // change, desktop registration or device change goes through changed()), the desktop or gamma state differs, or 30 s pass
  // (for the few entries read from the filesystem).
  GVariant* cachedCatalogVariant = nullptr;  // the catalog already converted for D-Bus, so a hit costs a reference
  std::uint64_t catalogEpoch = 0, cachedEpoch = ~std::uint64_t{0};
  bool cachedDesktop = false, cachedGamma = false;
  std::chrono::steady_clock::time_point cachedAt{};
  GVariant* catalog() {
    const auto now = std::chrono::steady_clock::now();
    if (cachedEpoch != catalogEpoch || cachedDesktop != desktop() || cachedGamma != gammaAvailable() || now - cachedAt > std::chrono::seconds(30)) {
      if (cachedCatalogVariant) g_variant_unref(cachedCatalogVariant);
      cachedCatalogVariant = g_variant_ref_sink(toVariant(settingsCatalog(config, desktop(), gammaAvailable())));
      cachedEpoch = catalogEpoch; cachedDesktop = desktop(); cachedGamma = gammaAvailable(); cachedAt = now;
    }
    return cachedCatalogVariant;
  }
  bool desktop() const { return !desktopOwner.empty(); }
  bool gammaAvailable() const { return desktop() && desktopCapabilities.value("gamma-control", false); }
  Json snapshot(const std::string& interface) {
    if (interface == displaysInterface) { displays->refresh(desktop()); return displays->snapshot(); }
    static const Json systemInfo = [] {
      Json details;
      auto* os = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
      details["os"] = os ? os : "Linux"; g_free(os);
      utsname kernel{};
      if (uname(&kernel) == 0) { details["kernel"] = kernel.release; details["architecture"] = kernel.machine; }
      return details;
    }();
    if (interface == configurationInterface) return {{"available", true}, {"helix-desktop", desktop()}, {"desktop-capabilities", desktopCapabilities},
      {"network", network != nullptr}, {"bluetooth", bluetooth != nullptr}, {"audio", audio != nullptr && !audioBroken}, {"system", systemInfo}};
    if (interface == appearanceInterface) {
      auto current = appearance; current["helix-desktop"] = desktop();
      current["wallpaper"] = desktop() ? config.getPaletteWallpaperPath() : std::string{};
      current["palette-previews"] = palettePreviews();
      current["wallhaven-key-available"] = desktop() && wallhaven.hasKey();
      current["wallhaven-online"] = desktop() && !config.config().shell.offlineMode;
      current["wallhaven-download-directory"] = desktop() ? wallhaven.downloadDirectory() : std::string{};
      return current;
    }
    if (interface == audioInterface) {
      if (!audio) return {{"available", false}};
      const auto& state = audio->state();
      return {{"available", audio->coreHandle() != nullptr && !audioBroken}, {"sinks", audioNodes(state.sinks)},
        {"sources", audioNodes(state.sources)}, {"programs", audioNodes(state.programOutputs)}};
    }
    if (interface == networkInterface) {
      if (!network) return {{"available", false}};
      const auto& state = network->state();
      Json aps = Json::array();
      for (const auto& ap : network->accessPoints()) aps.push_back({{"path", ap.path}, {"device", ap.devicePath},
        {"ssid", ap.ssid}, {"strength", ap.strength}, {"secured", ap.secured}, {"enterprise", ap.isEnterprise()},
        {"key-management", int(ap.keyManagement)}, {"active", ap.active}, {"frequency", ap.frequencyMhz},
        {"saved", network->hasSavedConnection(ap.ssid)}});
      Json vpns = Json::array();
      for (const auto& vpn : network->vpnConnections()) vpns.push_back({{"path", vpn.path}, {"name", vpn.name}, {"active", vpn.active}});
      return {{"available", network->hasStateSnapshot()}, {"enabled", state.wirelessEnabled},
        {"connected", state.connected}, {"resolving", state.resolving}, {"scanning", state.scanning},
        {"ssid", state.ssid}, {"address", state.ipv4}, {"interface", state.interfaceName},
        {"kind", int(state.kind)}, {"signal", state.signalStrength}, {"frequency", state.frequencyMhz},
        {"vpn-active", state.vpnActive}, {"vpn-connected", state.vpnConnected}, {"cellular-active", state.cellularActive},
        {"wired", network->canActivateWiredConnection()}, {"cellular", network->canActivateCellularConnection()},
        {"access-points", aps}, {"vpns", vpns}};
    }
    if (interface == bluetoothInterface) {
      if (!bluetooth) return {{"available", false}};
      const auto& state = bluetooth->state();
      Json devices = Json::array();
      for (const auto& device : bluetooth->devices()) devices.push_back({{"path", device.path}, {"address", device.address},
        {"name", device.alias}, {"kind", int(device.kind)}, {"paired", device.paired}, {"trusted", device.trusted},
        {"connected", device.connected}, {"connecting", device.connecting}, {"has-battery", device.hasBattery},
        {"battery", device.batteryPercent}, {"has-rssi", device.hasRssi}, {"rssi", device.rssi}});
      return {{"available", state.adapterPresent}, {"enabled", state.powered}, {"scanning", state.discovering},
        {"name", state.adapterName}, {"discoverable", state.discoverable}, {"pairable", state.pairable},
        {"soft-blocked", state.rfkillSoftBlocked}, {"hard-blocked", state.rfkillHardBlocked}, {"devices", devices}};
    }
    throw std::invalid_argument("Unknown interface");
  }
  std::uint64_t action(const std::string& interface, const std::string& name, const Json& args, const std::string& sender) {
    std::function<bool()> done;
    std::function<void()> cancel;
    if (interface == audioInterface) {
      if (!audio) throw std::runtime_error("Audio unavailable");
      const auto id = args.at("id").get<std::uint32_t>();
      const auto lookup = [this, id]() -> const AudioNode* {
        const auto& state = audio->state();
        for (const auto* nodes : {&state.sinks, &state.sources, &state.programOutputs})
          for (const auto& node : *nodes) if (node.id == id) return &node;
        return nullptr;
      };
      if (!lookup()) throw std::invalid_argument("Audio device is no longer available");
      if (name == "volume") {
        const double volume = args.at("value").get<double>();
        if (!std::isfinite(volume) || volume < 0 || volume > 1.5) throw std::invalid_argument("Volume out of range");
        const auto& state = audio->state();
        if (std::ranges::any_of(state.sources, [id](const auto& node) { return node.id == id; })) audio->setSourceVolume(id, float(volume));
        else if (std::ranges::any_of(state.sinks, [id](const auto& node) { return node.id == id; })) audio->setSinkVolume(id, float(volume));
        else audio->setProgramOutputVolume(id, float(volume));
        done = [lookup, volume] { const auto* node = lookup(); return node && std::abs(node->volume - volume) < 0.025; };
      } else if (name == "mute") {
        const bool muted = args.at("value").get<bool>();
        audio->setProgramOutputMuted(id, muted);
        done = [lookup, muted] { const auto* node = lookup(); return node && node->muted == muted; };
      } else if (name == "default") {
        const bool input = args.value("input", false);
        if (input) audio->setDefaultSource(id); else audio->setDefaultSink(id);
        done = [this, input, id] { return (input ? audio->state().defaultSourceId : audio->state().defaultSinkId) == id; };
      } else if (name == "route") {
        const auto sink = args.at("sink").get<std::uint32_t>();
        audio->moveProgramOutput(id, sink);
        done = [lookup, sink] { const auto* node = lookup(); return node && node->routeSinkId == sink; };
      } else throw std::invalid_argument("Unknown audio action");
    } else if (interface == networkInterface) {
      if (!network) throw std::runtime_error("NetworkManager unavailable");
      if (name == "enable") {
        const bool enabled = args.at("value").get<bool>(); network->setWirelessEnabled(enabled);
        done = [this, enabled] { return network->state().wirelessEnabled == enabled; };
      } else if (name == "scan") {
        network->requestScan(); done = [this] { return !network->state().scanning; };
      } else if (name == "connect" || name == "enterprise") {
        if (!networkOwner.empty()) throw std::runtime_error("Another client is connecting");
        const auto path = args.at("path").get<std::string>();
        const auto it = std::ranges::find(network->accessPoints(), path, &AccessPointInfo::path);
        if (it == network->accessPoints().end()) throw std::invalid_argument("Network is no longer available");
        const auto ssid = it->ssid;
        bool accepted = false;
        if (name == "enterprise") {
          network_enterprise::EnterpriseCredentials credentials;
          credentials.identity = args.at("identity"); credentials.anonymousIdentity = args.value("anonymous", "");
          credentials.password = args.at("secret"); credentials.caCertPath = args.value("certificate", "");
          credentials.domainSuffixMatch = args.value("domain", "");
          const int eap = args.value("eap", 0), phase2 = args.value("phase2", 0);
          if (eap < 0 || eap > 1 || phase2 < 0 || phase2 > 3) throw std::invalid_argument("Unknown enterprise authentication method");
          credentials.eap = network_enterprise::EapMethod(eap);
          credentials.phase2 = network_enterprise::Phase2Auth(phase2);
          if (network_enterprise::validate(credentials) != network_enterprise::Validation::Ok) throw std::invalid_argument("Invalid enterprise credentials");
          networkOwner = sender;
          accepted = network->activateEnterpriseAccessPoint(*it, credentials);
        } else { networkOwner = sender; accepted = args.contains("secret") ? network->activateAccessPoint(*it, args.at("secret").get<std::string>()) : network->activateAccessPoint(*it); }
        if (!accepted) { networkOwner.clear(); throw std::runtime_error("Connection was rejected"); }
        cancel = [this] { if (secrets) secrets->cancelSecret(); network->disconnect(); networkOwner.clear(); };
        done = [this, ssid] { return network->state().connected && network->state().ssid == ssid; };
      } else if (name == "disconnect") {
        network->disconnect(); if (secrets) secrets->cancelSecret(); networkOwner.clear();
        done = [this] { return !network->state().connected && !network->state().resolving; };
      } else if (name == "forget") {
        const auto ssid = args.at("ssid").get<std::string>(); network->forgetSsid(ssid);
        cancel = [this] { if (secrets) secrets->cancelSecret(); network->disconnect(); networkOwner.clear(); };
        done = [this, ssid] { return !network->hasSavedConnection(ssid); };
      } else if (name == "wired" || name == "cellular-connect" || name == "cellular-disconnect") {
        bool accepted = name == "wired" ? network->activateWiredConnection() : name == "cellular-connect" ? network->activateCellularConnection() : network->deactivateCellularConnection();
        if (!accepted) throw std::runtime_error("Connection rejected");
        done = [this, name] { return name == "wired" ? network->state().kind == NetworkConnectivity::Wired && network->state().connected : network->state().cellularActive == (name == "cellular-connect"); };
      } else if (name == "vpn-connect" || name == "vpn-disconnect") {
        const auto path = args.at("path").get<std::string>();
        const auto it = std::ranges::find(network->vpnConnections(), path, &VpnConnectionInfo::path);
        if (it == network->vpnConnections().end()) throw std::invalid_argument("Unknown VPN");
        bool accepted = name == "vpn-connect" ? network->activateVpnConnection(*it) : network->deactivateVpnConnection(*it);
        if (!accepted) throw std::runtime_error("VPN operation rejected");
        done = [this, path, enabled = name == "vpn-connect"] {
          const auto found = std::ranges::find(network->vpnConnections(), path, &VpnConnectionInfo::path);
          return found != network->vpnConnections().end() && found->active == enabled;
        };
      } else throw std::invalid_argument("Unknown network action");
    } else if (interface == bluetoothInterface) {
      if (!bluetooth) throw std::runtime_error("Bluetooth unavailable");
      if (name == "enable" || name == "discoverable" || name == "pairable") {
        const bool enabled = args.at("value").get<bool>();
        if (name == "enable") bluetooth->setPowered(enabled);
        if (name == "discoverable") bluetooth->setDiscoverable(enabled);
        if (name == "pairable") bluetooth->setPairable(enabled);
        done = [this, name, enabled] { const auto& state = bluetooth->state(); return
          (name == "enable" ? state.powered : name == "discoverable" ? state.discoverable : state.pairable) == enabled; };
      } else if (name == "scan" || name == "stop-scan") {
        if (name == "scan") bluetooth->startDiscovery(); else bluetooth->stopDiscovery();
        done = [this, enabled = name == "scan"] { return bluetooth->state().discovering == enabled; };
      } else {
        const auto path = args.at("path").get<std::string>();
        if (std::ranges::find(bluetooth->devices(), path, &BluetoothDeviceInfo::path) == bluetooth->devices().end())
          throw std::invalid_argument("Device is no longer available");
        if (name == "pair") {
          if (!bluetoothOwner.empty()) throw std::runtime_error("Another client is pairing");
          cancel = [this, path] { bluetooth->cancelPair(path); if (pairing) pairing->cancelPending(); bluetoothOwner.clear(); };
          bluetoothOwner = sender;
          if (!bluetooth->pair(path)) { bluetoothOwner.clear(); throw std::runtime_error("Pairing was rejected"); }
        } else if (name == "connect") { if (!bluetooth->connect(path)) throw std::runtime_error("Connection rejected"); }
        else if (name == "disconnect") { if (!bluetooth->disconnectDevice(path)) throw std::runtime_error("Disconnection rejected"); }
        else if (name == "forget") bluetooth->forget(path);
        else if (name == "cancel-pair") { bluetooth->cancelPair(path); if (pairing) pairing->cancelPending(); bluetoothOwner.clear(); }
        else if (name == "trust") bluetooth->setTrusted(path, args.at("value").get<bool>());
        else throw std::invalid_argument("Unknown Bluetooth action");
        done = [this, path, name, trusted = args.value("value", true)] {
          const auto it = std::ranges::find(bluetooth->devices(), path, &BluetoothDeviceInfo::path);
          if (name == "forget") return it == bluetooth->devices().end();
          if (name == "cancel-pair") return !pairing || !pairing->hasPendingRequest();
          if (it == bluetooth->devices().end()) return false;
          if (name == "pair") return it->paired;
          if (name == "trust") return it->trusted == trusted;
          return it->connected == (name == "connect");
        };
      }
    } else throw std::invalid_argument("Unknown action interface");
    const auto id = nextOperation++;
    operations.push_back({id, interface, sender, name, std::move(done), std::move(cancel), std::chrono::steady_clock::now() + std::chrono::seconds(45)});
    return id;
  }
  void method(const std::string& sender, const std::string& interface, const std::string& name,
              GVariant* parameters, GDBusMethodInvocation* invocation) {
    // A call that changes something gets a quick reaction for the next second. Reads do not: clients poll snapshots several times a
    // second, and counting those kept the service at the fast rate forever.
    const bool readOnly = name.starts_with("Get") || name.starts_with("List") || name.starts_with("Read");
    if (!readOnly) wake();
    if (interface == agentInterface) {
      if (agentEndpoint) agentEndpoint->method(sender, name, parameters, invocation);
      else g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Desktop.Error.Failed", "The agent surface is not running");
      return;
    }
    if (interface == agentActivityInterface) {
      if (agentEndpoint) agentEndpoint->activity(sender, name, parameters, invocation);
      else g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Desktop.Error.Failed", "The agent surface is not running");
      return;
    }
    try {
      const auto args = fromVariant(parameters);
      if (interface == audioInterface && (name == "StartInputMeter" || name == "StopInputMeter")) {
        if (name == "StopInputMeter") meters.erase(sender);
        else {
          if (!audio) throw std::runtime_error("Audio unavailable");
          const auto id = args.at(0).get<std::uint32_t>();
          if (std::ranges::find(audio->state().sources, id, &AudioNode::id) == audio->state().sources.end())
            throw std::invalid_argument("Microphone is no longer available");
          const auto expires = std::chrono::steady_clock::now() + std::chrono::seconds(8);
          if (meters.contains(sender) && meters.at(sender).source == id) meters.at(sender).expires = expires;
          else meters.insert_or_assign(sender, Meter{id, std::make_unique<InputMeter>(audio->coreHandle(), id), expires});
        }
        g_dbus_method_invocation_return_value(invocation, nullptr); return;
      }
      if (name == "Cancel" && (interface == networkInterface || interface == bluetoothInterface || interface == audioInterface)) {
        const auto id = args.at(0).get<std::uint64_t>();
        const auto it = std::ranges::find(operations, id, &Operation::id);
        if (it == operations.end() || it->interface != interface || it->sender != sender)
          throw std::invalid_argument("Operation is no longer available to this client");
        if (!it->cancel) throw std::invalid_argument("This operation cannot be cancelled");
        it->cancel();
        emit(interface.c_str(), "Completed", g_variant_new("(tbs)", id, false, "Cancelled"), sender);
        operations.erase(it); g_dbus_method_invocation_return_value(invocation, nullptr); return;
      }
      if (interface == networkInterface && (name == "GetWifiSecret" || name == "GetDns" || name == "SetDns")) {
        if (!network) throw std::runtime_error("NetworkManager unavailable");
        if (name == "GetWifiSecret") {
          if (sender != desktopOwner) throw std::invalid_argument("Only the registered desktop may request saved Wi-Fi credentials");
          g_object_ref(invocation);
          network->requestActiveWifiPsk([invocation](std::optional<std::string> secret) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", secret.value_or("").c_str())); g_object_unref(invocation);
          });
        } else if (name == "GetDns") {
          g_object_ref(invocation);
          network->requestActiveDnsOverride([invocation](DnsOverride value) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", toVariant({{"profile", int(value.profile)}, {"servers", value.servers}, {"nextdns", value.nextDnsId}}))); g_object_unref(invocation);
          });
        } else {
          const auto value = validatedDnsUpdate(args.at(0));
          g_object_ref(invocation);
          network->setDnsOverride(value, [invocation](bool success, std::string) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", toVariant({{"success", success}, {"message", success ? "" : "DNS could not be changed. Check the active connection and try again."}}))); g_object_unref(invocation);
          });
        }
        return;
      }
      if (name == "GetSnapshot") { g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", toVariant(snapshot(interface)))); return; }
      if (interface == configurationInterface) {
        if (name == "GetState" || name == "SetState" || name == "ClearState") {
          if (desktopOwner != sender) throw std::invalid_argument("Only the registered shell may access desktop runtime state");
          const auto owner = args.at(0).get<std::string>();
          if (name == "GetState") {
            const auto key = args.at(1).get<std::string>();
            Json output;
            if (args.at(2) == "boolean") { const auto value = config.stateBool(owner, key); output = {{"present", value.has_value()}, {"value", value.value_or(false)}}; }
            else if (args.at(2) == "string") { const auto value = config.stateString(owner, key); output = {{"present", value.has_value()}, {"value", value.value_or("")}}; }
            else throw std::invalid_argument("Unknown runtime state type");
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", toVariant(output))); return;
          }
          if (name == "SetState") {
            const auto key = args.at(1).get<std::string>(); const auto& value = args.at(2);
            if (!value.is_boolean() && !value.is_string()) throw std::invalid_argument("Runtime state must be boolean or string");
            const bool ok = value.is_boolean() ? config.setStateBool(owner, key, value.get<bool>()) : config.setStateString(owner, key, value.get<std::string>());
            if (!ok) throw std::runtime_error("Runtime state could not be saved");
          } else if (!config.clearStateOwner(owner)) throw std::runtime_error("Runtime state could not be cleared");
          g_dbus_method_invocation_return_value(invocation, nullptr); return;
        }
        if (name == "GetSettings") { g_dbus_method_invocation_return_value(invocation, g_variant_new("(@av)", g_variant_ref(catalog()))); return; }
        if (name == "RegisterDesktop") {
          if (!desktopOwner.empty() && desktopOwner != sender) throw std::runtime_error("A Helix desktop is already registered");
          desktopOwner = sender; theme.apply(); changed(configurationInterface);
        } else if (name == "RegisterCapabilities") {
          if (desktopOwner != sender) throw std::invalid_argument("Only the registered desktop may report capabilities");
          const auto capabilities = args.at(0);
          if (!capabilities.is_object()) throw std::invalid_argument("Capabilities must be an object");
          for (const auto& [key, value] : capabilities.items())
            if (key != "gamma-control" || !value.is_boolean()) throw std::invalid_argument("Unknown desktop capability");
          if (desktopCapabilities != capabilities) { desktopCapabilities = capabilities; changed(configurationInterface); }
        } else if (name == "CommitOverrides") {
          if (desktopOwner != sender) throw std::invalid_argument("Only the registered shell may commit a configuration replica");
          if (!config.commitRemoteOverrides(args.at(0).get<std::string>(), args.at(1).get<std::string>())) throw std::runtime_error(config.lastMutationError());
        } else if (name == "UpdateApplicationList" || name == "UpdateOrderedList") {
          const auto id = args.at(0).get<std::string>();
          if (!desktop()) throw std::invalid_argument("Ordered desktop lists require Helix desktop");
          bool supported = false;
          Json setting;
          for (const auto& entry : settingsCatalog(config, true, gammaAvailable())) if (entry.at("id") == id) {
            setting = entry;
            supported = entry.at("type") == "application-list" || (name == "UpdateOrderedList" && (entry.at("type") == "widget-list" || entry.at("type") == "display-list" || entry.at("type") == "notification-rules" || entry.at("type") == "tab-list" || entry.at("type") == "shortcut-list" || entry.at("type") == "key-chords" || entry.at("type") == "template-list" || entry.at("type") == "transition-list"));
          }
          if (!supported) throw std::invalid_argument("Unknown ordered desktop list");
          config.forceReload();
          validateSettingValue(config, id, args.at(2), true, gammaAvailable());
          for (const auto& entry : settingsCatalog(config, true, gammaAvailable())) if (entry.at("id") == id && entry.at("value") != args.at(1))
            throw std::invalid_argument("The list changed. Refresh and try again");
          bool saved = false;
          if (setting.at("type") == "tab-list") {
            std::set<std::string> builtIn;
            for (const auto& option : setting.at("options")) builtIn.insert(option.get<std::string>());
            // These legacy aliases also hide built-in pages in the shell.
            builtIn.insert({"ai-usage", "screenshot", "screen-record"});
            std::vector<std::string> hidden;
            for (const auto& tab : config.config().controlCenter.hiddenTabs) if (!builtIn.contains(tab)) hidden.push_back(tab);
            for (const auto& option : setting.at("options")) if (std::ranges::find(args.at(2), option) == args.at(2).end()) hidden.push_back(option.get<std::string>());
            saved = config.setOverride(settingPath(id), std::move(hidden));
          } else saved = config.setOverride(settingPath(id), settingValue(args.at(2), id));
          if (!saved) throw std::runtime_error("Ordered settings could not be saved");
        } else if (name == "Update" || name == "Reset") {
          const auto id = args.at(0).get<std::string>();
          if (!desktop() && !isPortableSetting(id)) throw std::invalid_argument("This setting requires Helix desktop");
          if (id.starts_with("nightlight.") && !gammaAvailable()) throw std::invalid_argument("Night Light is unavailable on this desktop");
          config.forceReload();
          if (name == "Update" && id.starts_with("keybinds.")) throw std::invalid_argument("Keyboard shortcuts require UpdateOrderedList");
          if (name == "Update" && id == "control_center.shortcuts") throw std::invalid_argument("Quick shortcuts require UpdateOrderedList");
          if (name == "Update" && (id == "theme.templates.builtin_ids" || id == "theme.templates.community_ids")) throw std::invalid_argument("Template selections require UpdateOrderedList");
          if (name == "Update" && id == "wallpaper.transition") throw std::invalid_argument("Wallpaper transitions require UpdateOrderedList");
          if (name == "Update") validateSettingValue(config, id, args.at(1), desktop(), gammaAvailable());
          bool ok;
          if (id == "nightlight.temperature_day" || id == "nightlight.temperature_night") {
            if (!gammaAvailable()) throw std::invalid_argument("Night Light is unavailable on this desktop");
            if (name == "Reset") {
              bool changed = false;
              ok = config.clearOverrides({{"nightlight", "temperature_day"}, {"nightlight", "temperature_night"}}, &changed);
            } else {
              std::int64_t day = config.config().nightlight.dayTemperature, night = config.config().nightlight.nightTemperature;
              if (id == "nightlight.temperature_day") { day = args.at(1).get<std::int64_t>(); night = std::min(night, day - NightLightConfig::kTemperatureGap); }
              else { night = args.at(1).get<std::int64_t>(); day = std::max(day, night + NightLightConfig::kTemperatureGap); }
              ok = config.setOverrides({{{"nightlight", "temperature_day"}, day}, {{"nightlight", "temperature_night"}, night}});
            }
          } else if (name == "Update" && id.starts_with("idle:")) {
            const auto path = settingPath(id); auto behaviors = config.config().idle.behaviors;
            for (auto& behavior : behaviors) if (behavior.name == path.at(2)) {
              const auto& value = args.at(1); const auto& field = path.at(3);
              if (field == "enabled") behavior.enabled = value.get<bool>();
              else if (field == "timeout") behavior.timeoutSeconds = value.get<double>();
              else if (field == "locked_timeout") behavior.lockedTimeoutSeconds = value.get<double>();
              else if (field == "lock_before_suspend") behavior.lockBeforeSuspend = value.get<bool>();
              else if (field == "action") { behavior.action = value.get<std::string>(); if (behavior.action == "suspend") behavior.lockBeforeSuspend = false; }
            }
            ok = config.setOverride({"idle", "behavior"}, std::move(behaviors));
          } else if (name == "Reset") {
            // Resetting a setting that is already at its default is not a failure; clearOverride() reports "nothing changed" as false.
            bool changed = false;
            ok = config.clearOverrides({settingPath(id)}, &changed);
          } else ok = config.setOverride(settingPath(id), settingValue(args.at(1), id));
          if (!ok) throw std::invalid_argument(config.lastMutationError().empty() ? "Setting could not be saved" : config.lastMutationError());
        } else throw std::invalid_argument("Unknown configuration method");
      } else if (interface == appearanceInterface && (name == "SavePalette" || name == "SetRole")) {
        std::string paletteName;
        bool ok = false;
        if (name == "SavePalette") {
          if (args.at(0) == "wallpaper") ok = theme.saveWallpaperPaletteAsCustom(&paletteName);
          else if (args.at(0) == "current") ok = theme.saveCurrentPaletteAsCustom(&paletteName);
          else throw std::invalid_argument("Unknown palette source");
        } else {
          auto role = args.at(0).get<unsigned>();
          if (role >= kColorRoleTokens.size()) throw std::invalid_argument("Unknown color role");
          ok = theme.saveRoleColorAsCustom(ColorRole(role), hex(args.at(1).get<std::string>()), &paletteName);
        }
        if (!ok) throw std::runtime_error("Palette could not be saved");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", paletteName.c_str())); return;
      } else if (interface == appearanceInterface && name == "RefreshPalettes") {
        paletteCatalog.sync();
      } else if (interface == appearanceInterface && name == "Reapply") {
        if (!desktop()) throw std::invalid_argument("Appearance exports require Helix desktop");
        if (!templates.reapplyLast()) throw std::runtime_error("Appearance is not ready");
      } else if (interface == appearanceInterface && name == "SetAccent") {
        const auto color = hex(args.at(0).get<std::string>());
        std::string paletteName;
        if (!theme.saveRoleColorAsCustom(ColorRole::Primary, color, &paletteName)) throw std::runtime_error("Accent could not be saved");
      } else if (interface == appearanceInterface && name == "SetWallhavenApiKey") {
        if (!desktop()) throw std::invalid_argument("Wallhaven settings require a Helix desktop");
        const auto key = args.at(0).get<std::string>();
        if (key.empty() || key.size() > 256 || !std::ranges::all_of(key, [](unsigned char c) { return g_ascii_isalnum(c); }))
          throw std::invalid_argument("Enter a valid Wallhaven API key");
        if (!config.setOverride({"wallpaper", "wallhaven", "api_key"}, key)) throw std::runtime_error("Wallhaven API key could not be saved");
      } else if (interface == appearanceInterface && name == "SearchWallhaven") {
        const auto id = wallhaven.search(args.at(0), sender);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(t)", id)); return;
      } else if (interface == appearanceInterface && name == "GetWallhaven") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", toVariant(wallhaven.result(args.at(0).get<std::uint64_t>(), sender)))); return;
      } else if (interface == appearanceInterface && name == "DownloadWallhaven") {
        const auto id = wallhaven.download(args.at(0).get<std::uint64_t>(), args.at(1).get<std::string>(), sender);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(t)", id)); return;
      } else if (interface == appearanceInterface && name == "CancelWallhaven") {
        wallhaven.cancel(args.at(0).get<std::uint64_t>(), sender);
      } else if (interface == appearanceInterface && name == "SetWallpaper") {
        if (!desktop()) throw std::invalid_argument("Wallpaper requires Helix desktop");
        const auto path = args.at(0).get<std::string>();
        if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("Choose an existing wallpaper file");
        config.setWallpaperPath(std::nullopt, path);
      } else if (name == "Action") {
        const auto id = action(interface, args.at(0).get<std::string>(), args.at(1), sender);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(t)", id)); return;
      } else if (name == "Reply") {
        const auto response = args.at(0).get<std::string>();
        const bool cancel = args.at(1).get<bool>();
        if (interface == networkInterface) {
          if (networkOwner != sender || !secrets || !secrets->hasPendingRequest()) throw std::invalid_argument("No credential request for this client");
          if (cancel) secrets->cancelSecret(); else secrets->submitSecret(response);
        } else if (interface == bluetoothInterface) {
          if (bluetoothOwner != sender || !pairing || !pairing->hasPendingRequest()) throw std::invalid_argument("No pairing request for this client");
          if (cancel) pairing->rejectConfirm();
          else {
            const auto kind = pairing->pendingRequest().kind;
            if (kind == BluetoothPairingKind::PinCode) pairing->submitPin(response);
            else if (kind == BluetoothPairingKind::Passkey) {
              if (response.empty() || response.size() > 6 || !std::ranges::all_of(response, [](char c) { return c >= '0' && c <= '9'; })) throw std::invalid_argument("Passkey must contain at most six digits");
              pairing->submitPasskey(static_cast<std::uint32_t>(std::stoul(response)));
            } else pairing->acceptConfirm();
          }
        } else throw std::invalid_argument("Unknown reply interface");
        if (cancel) {
          for (auto it = operations.begin(); it != operations.end();) {
            const bool authentication = it->action == "pair" || it->action == "connect" || it->action == "enterprise";
            if (it->interface == interface && it->sender == sender && authentication && it->cancel) {
              it->cancel();
              emit(interface.c_str(), "Completed", g_variant_new("(tbs)", it->id, false, "Cancelled"), sender);
              it = operations.erase(it);
            } else ++it;
          }
        }
      } else throw std::invalid_argument("Unknown desktop method");
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } catch (const std::invalid_argument& error) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Desktop.Error.InvalidArgument", error.what());
    } catch (const std::exception& error) {
      // Backend exception text may contain submitted credentials; expose a fixed error.
      const auto message = (name == "Update" || name == "Reset" || name == "CommitOverrides") ? "Settings could not be saved. Refresh and try again." : "The operation failed. Check the device state and try again.";
      g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Desktop.Error.Failed", message);
    }
  }
  void wake() {
    busyUntil = std::chrono::steady_clock::now() + kBusyHold;
    if (source && tickIntervalMs != kBusyIntervalMs) setTickInterval(kBusyIntervalMs);
  }
  void setTickInterval(guint interval) {
    if (source) g_source_remove(source);
    tickIntervalMs = interval;
    source = g_timeout_add(interval, [](gpointer data) -> gboolean { return static_cast<Service*>(data)->tickAndRearm(); }, this);
  }
  // Returns whether this timer source stays. Switching speed replaces the source, so the old one is removed here.
  gboolean tickAndRearm() {
    tick();
    const auto now = std::chrono::steady_clock::now();
    const bool busy = now < busyUntil || !meters.empty() || !operations.empty();
    const guint wanted = busy ? kBusyIntervalMs : kIdleIntervalMs;
    if (wanted == tickIntervalMs) return G_SOURCE_CONTINUE;
    tickIntervalMs = wanted;
    source = g_timeout_add(wanted, [](gpointer data) -> gboolean { return static_cast<Service*>(data)->tickAndRearm(); }, this);
    return G_SOURCE_REMOVE;
  }
  void tick() {
    try {
      displays->refresh(desktop());
      const auto backendNow = std::chrono::steady_clock::now();
      if (backendNow >= nextBackendCheck) {
        nextBackendCheck = backendNow + std::chrono::seconds(3); recoverBackends();
      }
      if (system) system->processPendingEvents();
      if (audio) audio->dispatch();
      std::erase_if(meters, [backendNow](const auto& meter) { return backendNow >= meter.second.expires; });
      if (backendNow >= nextMeterUpdate) {
        nextMeterUpdate = backendNow + std::chrono::milliseconds(80);
        for (const auto& [owner, meter] : meters)
          emit(audioInterface, "InputLevel", g_variant_new("(udb)", meter.source, meter.capture->peak(), meter.capture->available()), owner);
      }
      std::vector<pollfd> fds;
      std::size_t mixerStart = 0;
      if (mixer) mixerStart = mixer->addPollFds(fds);
      const auto httpStart = fds.size(); http.addPollFds(fds);
      if (!fds.empty()) ::poll(fds.data(), fds.size(), 0);
      for (const auto& fd : fds)
        if (fd.revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) { busyUntil = std::chrono::steady_clock::now() + kBusyHold; break; }
      if (fds.size() > httpStart) busyUntil = std::chrono::steady_clock::now() + kBusyHold;  // an HTTP transfer is running
      if (mixer) mixer->dispatch(fds, mixerStart);
      http.dispatch(fds, httpStart);
      DeferredCall::drainWakeFd();
      auto deferred = DeferredCall::takePending();
      if (!deferred.empty()) busyUntil = std::chrono::steady_clock::now() + kBusyHold;
      for (auto& callback : deferred) callback();
      TimerManager::instance().tick();
      config.checkReload();
      wallhaven.expire();
      if (agentEndpoint) agentEndpoint->tick();
      const auto now = std::chrono::steady_clock::now();
      for (auto it = operations.begin(); it != operations.end();) {
        const bool success = it->done();
        if (success || now >= it->deadline) {
          emit(it->interface.c_str(), "Completed", g_variant_new("(tbs)", it->id, success, success ? "" : "Operation timed out"), it->sender);
          if (it->interface == networkInterface && networkOwner == it->sender && (it->action == "connect" || it->action == "enterprise")) { if (secrets) secrets->cancelSecret(); networkOwner.clear(); }
          if (it->interface == bluetoothInterface && bluetoothOwner == it->sender && it->action == "pair") { if (pairing) pairing->cancelPending(); bluetoothOwner.clear(); }
          it = operations.erase(it);
        } else ++it;
      }
    } catch (const std::exception&) { g_warning("Desktop backend dispatch failed"); }
  }
  void start(GDBusConnection* connection) {
    bus = G_DBUS_CONNECTION(g_object_ref(connection));
    displays->changed = [this] { changed(displaysInterface); };
    initialize();
    GError* error = nullptr;
    info = g_dbus_node_info_new_for_xml(introspection, &error);
    if (!info) throw std::runtime_error(error ? error->message : "Invalid service interfaces");
    static const GDBusInterfaceVTable table = {
      [](GDBusConnection*, const gchar* sender, const gchar*, const gchar* interface, const gchar* name,
         GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
        static_cast<Service*>(data)->method(sender, interface, name, parameters, invocation);
      }, nullptr, nullptr, {nullptr}
    };
    for (auto** interface = info->interfaces; *interface; ++interface) {
      auto id = g_dbus_connection_register_object(bus, objectPath, *interface, &table, this, nullptr, &error);
      if (!id) throw std::runtime_error(error ? error->message : "Service registration failed");
      registrations.push_back(id);
    }
    agentEndpoint = std::make_unique<agent::AgentEndpoint>(bus, objectPath, agentInterface, [this] { return desktopOwner; });
    ownerWatch = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "NameOwnerChanged", "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* value, gpointer data) {
        auto& service = *static_cast<Service*>(data);
        const gchar *name, *oldOwner, *newOwner;
        g_variant_get(value, "(&s&s&s)", &name, &oldOwner, &newOwner);
        if (*newOwner) return;
        service.wallhaven.disconnect(name);
        if (service.desktopOwner == name) { service.desktopOwner.clear(); service.desktopCapabilities = Json::object(); service.changed(configurationInterface); }
        if (service.networkOwner == name) { if (service.secrets) service.secrets->cancelSecret(); service.networkOwner.clear(); }
        if (service.bluetoothOwner == name) { if (service.pairing) service.pairing->cancelPending(); service.bluetoothOwner.clear(); }
        service.meters.erase(name);
        if (service.agentEndpoint) service.agentEndpoint->ownerGone(name);
        std::erase_if(service.operations, [name](const auto& operation) { return operation.sender == name; });
      }, this, nullptr);
    wake();
    setTickInterval(kBusyIntervalMs);
  }
};
}
}
int main() {
  helix::Service service;
  service.loop = g_main_loop_new(nullptr, false);
  const auto ownership = g_bus_own_name(G_BUS_TYPE_SESSION, helix::busName, G_BUS_NAME_OWNER_FLAGS_NONE,
    nullptr, [](GDBusConnection* bus, const gchar*, gpointer data) {
      try { static_cast<helix::Service*>(data)->start(bus); }
      catch (const std::exception& error) { g_printerr("Helix service could not start: %s\n", error.what()); g_main_loop_quit(static_cast<helix::Service*>(data)->loop); }
    },
    [](GDBusConnection*, const gchar*, gpointer data) { g_main_loop_quit(static_cast<helix::Service*>(data)->loop); }, &service, nullptr);
  auto stop = [](gpointer data) -> gboolean { g_main_loop_quit(static_cast<helix::Service*>(data)->loop); return G_SOURCE_CONTINUE; };
  const auto term = g_unix_signal_add(SIGTERM, stop, &service);
  const auto interrupt = g_unix_signal_add(SIGINT, stop, &service);
  g_main_loop_run(service.loop);
  g_source_remove(term); g_source_remove(interrupt);
  g_bus_unown_name(ownership);
  return service.bus ? 0 : 1;
}
