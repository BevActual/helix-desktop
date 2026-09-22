#pragma once

#include "core/timer_manager.h"
#include "ui/signal.h"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class SessionBus;
class IpcService;

struct PhoneDevice {
  std::string id;
  std::string name;
  std::string type;
  bool reachable = false;
  bool paired = false;
  bool pairRequested = false;
  bool pairRequestedByPeer = false;
  std::string verificationKey;
  bool hasBattery = false;
  int charge = -1;
  bool charging = false;
  bool sharePlugin = false;
  bool sftpPlugin = false;
  bool notificationsPlugin = false;
  std::vector<std::string> reachableAddresses;

  bool operator==(const PhoneDevice&) const = default;
};

struct PhoneContinueItem {
  std::string url;
  std::string title;
  std::string deviceId;
  std::string deviceName;
  std::int64_t timestamp = 0;
  bool isFile = false;
  bool isImage = false;

  bool operator==(const PhoneContinueItem&) const = default;
};

struct PhoneNotification {
  std::string deviceId;
  std::string publicId;
  std::string appName;
  std::string title;
  std::string text;
  std::string ticker;
  std::string smsCode;
  bool dismissable = false;

  bool operator==(const PhoneNotification&) const = default;
};

struct PhoneSftpSnapshot {
  std::string deviceId;
  bool supported = false;
  bool mounting = false;
  bool mounted = false;
  std::string error;
  std::vector<std::pair<std::string, std::string>> directories;
};

struct PhoneCastApp {
  std::string package;
  std::string label;
  std::string iconPath;

  bool operator==(const PhoneCastApp&) const = default;
};

// One thing the iPhone mirror needs that this machine lacks, with the package or
// step that fixes it. Shown one per line on the Cast card.
struct PhoneMirrorRequirement {
  std::string label; // translated
  std::string fix;   // translated hint (package name, setting)

  bool operator==(const PhoneMirrorRequirement&) const = default;
};

struct PhoneCastSnapshot {
  std::string deviceId;
  bool loading = false;
  bool toolsReady = false;
  bool preflightDone = false; // uxplay / GStreamer decoder / Bluetooth LE probes have run
  std::string decoder;        // GStreamer H.264 decoder the receiver will use
  std::vector<PhoneMirrorRequirement> missing;
  bool adbConnected = false;
  bool connecting = false;
  bool applePeer = false;
  bool airplayRunning = false;   // UxPlay receiver is up (Helix Phone listed in Screen Mirroring)
  bool receiverAuto = false;     // receiver was armed by [phone] mirror_always_on, not the button
  bool mirrorStreaming = false;  // the iPhone is sending frames right now
  // Helix Phone BLE pointer (iPhone AssistiveTouch), read from helix-phone-hid.py.
  bool pointerRunning = false;   // control socket answered
  bool pointerConnected = false; // iPhone subscribed to input reports
  bool pointerKeyboard = false;  // mirror holds the keyboard after a click
  bool pointerConfigOk = true;   // bluetoothd has Privacy on + JustWorksRepairing always
  bool pointerBonded = false;    // some device is paired with the Helix Phone adapter
  bool pointerBondLost = false;  // bonded, mirroring, yet not connected for a while
  std::string adbSerial;
  std::string suggestedHost;
  std::string airplayName;
  std::string error;
  std::vector<PhoneCastApp> apps;
  std::vector<PhoneCastApp> recent;

  bool operator==(const PhoneCastSnapshot&) const = default;
};

struct PhoneSnapshot {
  bool available = false;
  bool activating = false;
  std::string announcedName;
  std::vector<PhoneDevice> devices;
  std::string error;

  bool operator==(const PhoneSnapshot&) const = default;
};

class PhoneConnectService {
public:
  using ChangeCallback = std::function<void()>;

  explicit PhoneConnectService(SessionBus& bus);
  ~PhoneConnectService();

  PhoneConnectService(const PhoneConnectService&) = delete;
  PhoneConnectService& operator=(const PhoneConnectService&) = delete;

  [[nodiscard]] const PhoneSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool available() const noexcept { return m_snapshot.available; }

  void setChangeCallback(ChangeCallback callback);
  [[nodiscard]] Signal<>& changed() noexcept { return m_changed; }
  void start();
  void refresh();
  void scheduleRefresh();
  void tryActivate();
  void setDiscovery(bool enabled);

  void requestPair(const std::string& deviceId);
  void acceptPair(const std::string& deviceId);
  void rejectPair(const std::string& deviceId);
  void unpair(const std::string& deviceId);
  void ping(const std::string& deviceId);
  void ring(const std::string& deviceId);
  void shareUrl(const std::string& deviceId, std::string url);
  void shareText(const std::string& deviceId, std::string text);
  void sharePath(const std::string& deviceId, std::string path);

  void mountFiles(const std::string& deviceId);
  void refreshFiles(const std::string& deviceId);
  [[nodiscard]] const PhoneSftpSnapshot& filesSnapshot() const noexcept { return m_files; }
  [[nodiscard]] const std::vector<PhoneContinueItem>& continueItems() const noexcept { return m_continueItems; }
  [[nodiscard]] const std::vector<PhoneNotification>& notifications() const noexcept { return m_notifications; }
  [[nodiscard]] const PhoneDevice* peerDevice(std::string_view preferredId = {}) const;
  void refreshNotifications(const std::string& deviceId);
  void dismissNotification(const std::string& deviceId, const std::string& publicId);
  [[nodiscard]] std::optional<std::filesystem::path> pullFile(const std::string& sourcePath);
  void recordShare(const std::string& deviceId, std::string url);

  [[nodiscard]] const PhoneCastSnapshot& castSnapshot() const noexcept { return m_cast; }
  [[nodiscard]] bool castSupportedFor(const PhoneDevice& device) const noexcept;
  [[nodiscard]] bool castAndroidFor(const PhoneDevice& device) const noexcept;
  [[nodiscard]] bool castAppleFor(const PhoneDevice& device) const noexcept;
  [[nodiscard]] std::string castSuggestedHost(const std::string& deviceId) const;
  void refreshCastApps(const std::string& deviceId);
  void connectCastAdb(const std::string& deviceId, std::string host, std::string pairCode = {});
  void launchCastMirror(const std::string& deviceId);
  void stopCastMirror();
  // [phone] mirror_always_on: keep the AirPlay receiver and the Bluetooth pointer up
  // whenever an iPhone is paired, so mirroring is one tap on the phone.
  void setMirrorAlwaysOn(bool enabled);
  // Ends the phone's current session (UxPlay has no per-client kick) and, when the
  // receiver is always-on, brings it straight back.
  void restartReceiver();
  // iPhone pointer: named shortcut ("home", "search", "library", "siri", "control_center",
  // "notifications") sent to the Helix Phone HID script; bluetoothd setup runs via pkexec.
  void phonePointerShortcut(const std::string& name);
  void setupPhonePointerBluetooth();
  void launchCastApp(const std::string& deviceId, const std::string& package, const std::string& label);

  void registerIpc(IpcService& ipc);

private:
  struct Impl;
  friend struct Impl;

  void attachDaemon();
  void detachDaemon();
  void notifyChanged();
  [[nodiscard]] PhoneDevice* findDevice(const std::string& deviceId);
  [[nodiscard]] const PhoneDevice* findDevice(const std::string& deviceId) const;
  [[nodiscard]] const PhoneDevice* resolveDevice(std::string_view query) const;

  SessionBus& m_bus;
  PhoneSnapshot m_snapshot;
  PhoneSftpSnapshot m_files;
  PhoneCastSnapshot m_cast;
  std::vector<PhoneContinueItem> m_continueItems;
  std::vector<PhoneNotification> m_notifications;
  std::unordered_map<std::string, std::string> m_castAdbHosts;
  std::unordered_map<std::string, std::vector<PhoneCastApp>> m_castRecent;
  bool m_castRefreshInFlight = false;
  PhoneCastSnapshot m_castPending;
  bool m_castPendingReady = false;
  mutable std::mutex m_castMutex;
  Signal<> m_changed;
  Signal<>::ScopedConnection m_legacyChangeConn;
  Timer m_refreshTimer;
  Timer m_castPollTimer;
  Timer m_castIconPollTimer;
  bool m_castIconInFlight = false;
  std::vector<std::pair<std::string, std::string>> m_castIconPending; // package, path
  bool m_castIconPendingReady = false;
  // UxPlay runs on a process worker so its log can be read for session start/stop;
  // the cancel flag ends it, the generation ignores exits from a previous run.
  std::shared_ptr<std::atomic<bool>> m_airplayCancel;
  bool m_airplayAlive = false;
  std::uint32_t m_airplayGeneration = 0;
  bool m_mirrorAlwaysOn = true;
  bool m_receiverUserStopped = false; // Stop pressed: no auto re-arm until Start
  bool m_armPending = false;
  bool m_destroying = false;
  Timer m_airplayPollTimer;
  Timer m_receiverRearmTimer;
  void armReceiverIfWanted();
  void onAirplayLog(std::uint32_t generation, std::string line);
  void onAirplayExit(std::uint32_t generation, int exitCode);
  bool m_pointerSetupInFlight = false;
  // When the pointer was last seen connected (or the mirror started); drives bond-lost.
  std::chrono::steady_clock::time_point m_pointerLastSeen{};
  // Mirror preflight runs once per session on a worker (gst-inspect can take a while
  // the first time); the result is copied into every Apple cast snapshot.
  struct MirrorPreflight {
    bool done = false;
    bool inFlight = false;
    std::string decoder;
    std::vector<PhoneMirrorRequirement> missing;
  };
  MirrorPreflight m_preflight;
  void runMirrorPreflight();
  void applyMirrorPreflight();
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
  std::unique_ptr<Impl> m_impl;

  void loadContinueHistory();
  void saveContinueHistory();
  void loadCastState();
  void saveCastState();
  void saveCastAdbHost(const std::string& deviceId, const std::string& host);
  void recordCastRecent(const std::string& deviceId, const std::string& package, const std::string& label);
  [[nodiscard]] std::vector<std::string> castHostCandidates(const std::string& deviceId) const;
  [[nodiscard]] std::optional<std::string> resolveCastSerial(const std::string& deviceId);
  void queueCastRefreshResult(PhoneCastSnapshot snapshot);
  void drainCastRefreshResult();
  void attachCachedCastIcons(std::vector<PhoneCastApp>& apps, const std::string& deviceId) const;
  void scheduleCastIconFetch(const std::string& deviceId, const std::string& serial, std::vector<PhoneCastApp> apps);
  void drainCastIconResults();
  void pollAppleMirror();
  void startAppleMirror(bool automatic = false);
  void pollPhonePointer();
  [[nodiscard]] bool applyPhonePointerStatus(const std::optional<nlohmann::json>& status);
  bool m_pointerProbeInFlight = false;
};
