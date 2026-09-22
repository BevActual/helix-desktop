#include "dbus/kdeconnect/phone_connect_service.h"

#include "dbus/bus_name_watch.h"

#include "cli/schema_msg.h"
#include "compositors/niri/niri_phone_cast.h"
#include "core/deferred_call.h"
#include "core/files/directory_scanner.h"
#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "shell/phone_mirror/phone_mirror_window.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("phone");

  const sdbus::ServiceName kKdeConnectBusName{"org.kde.kdeconnect"};
  const sdbus::ObjectPath kDaemonPath{"/modules/kdeconnect"};
  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};

  constexpr auto kDbusInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kDaemonInterface = "org.kde.kdeconnect.daemon";
  constexpr auto kDeviceInterface = "org.kde.kdeconnect.device";
  constexpr auto kBatteryInterface = "org.kde.kdeconnect.device.battery";
  constexpr auto kPingInterface = "org.kde.kdeconnect.device.ping";
  constexpr auto kFindMyPhoneInterface = "org.kde.kdeconnect.device.findmyphone";
  constexpr auto kShareInterface = "org.kde.kdeconnect.device.share";
  constexpr auto kSftpInterface = "org.kde.kdeconnect.device.sftp";
  constexpr auto kNotificationsInterface = "org.kde.kdeconnect.device.notifications";
  constexpr auto kNotificationItemInterface = "org.kde.kdeconnect.device.notifications.notification";
  constexpr auto kSharePlugin = "kdeconnect_share";
  constexpr auto kSftpPlugin = "kdeconnect_sftp";
  constexpr auto kNotificationsPlugin = "kdeconnect_notifications";

  constexpr auto kDiscoveryToken = "helix-shell";
  constexpr auto kRefreshDebounce = std::chrono::milliseconds(150);

  using InterfaceProps = std::map<std::string, sdbus::Variant>;

  template <typename T> std::optional<T> variantGet(const sdbus::Variant& value) {
    try {
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  bool isUnknownMethod(const sdbus::Error& err) {
    return err.getName() == sdbus::Error::Name{"org.freedesktop.DBus.Error.UnknownMethod"};
  }

  std::string deviceObjectPath(const std::string& id) { return "/modules/kdeconnect/devices/" + id; }

  std::string pluginObjectPath(const std::string& id, std::string_view plugin) {
    return deviceObjectPath(id) + "/" + std::string(plugin);
  }

  void mergeDeviceProps(const InterfaceProps& props, PhoneDevice& out) {
    if (auto it = props.find("id"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second); v && !v->empty()) {
        out.id = std::move(*v);
      }
    }
    if (auto it = props.find("name"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        out.name = std::move(*v);
      }
    }
    if (auto it = props.find("type"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        out.type = std::move(*v);
      }
    }
    if (auto it = props.find("isReachable"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.reachable = *v;
      }
    }
    if (auto it = props.find("isPaired"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.paired = *v;
      }
    } else if (auto trusted = props.find("isTrusted"); trusted != props.end()) {
      if (auto v = variantGet<bool>(trusted->second)) {
        out.paired = *v;
      }
    }
    if (auto it = props.find("isPairRequested"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.pairRequested = *v;
      }
    }
    if (auto it = props.find("isPairRequestedByPeer"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.pairRequestedByPeer = *v;
      }
    }
    if (auto it = props.find("verificationKey"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        out.verificationKey = std::move(*v);
      }
    }
    if (auto it = props.find("reachableAddresses"); it != props.end()) {
      if (auto v = variantGet<std::vector<std::string>>(it->second)) {
        out.reachableAddresses = std::move(*v);
      }
    }
  }

  void mergeBatteryProps(const InterfaceProps& props, PhoneDevice& out) {
    if (auto it = props.find("hasBattery"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.hasBattery = *v;
      }
    }
    if (auto it = props.find("charge"); it != props.end()) {
      if (auto v = variantGet<std::int32_t>(it->second)) {
        out.charge = *v;
        if (out.charge >= 0) {
          out.hasBattery = true;
        }
      }
    }
    if (auto it = props.find("isCharging"); it != props.end()) {
      if (auto v = variantGet<bool>(it->second)) {
        out.charging = *v;
      }
    }
  }

  std::string fileUrlFromPath(std::string_view path) {
    std::string out = "file://";
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const char raw : path) {
      const auto c = static_cast<unsigned char>(raw);
      const bool keep = c == '/'
          || (c >= 'A' && c <= 'Z')
          || (c >= 'a' && c <= 'z')
          || (c >= '0' && c <= '9')
          || c == '-'
          || c == '_'
          || c == '.'
          || c == '~';
      if (keep) {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('%');
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
      }
    }
    return out;
  }

  [[nodiscard]] std::filesystem::path continueStatePath() {
    return std::filesystem::path(FileUtils::stateDir()) / "phone-continue.json";
  }

  [[nodiscard]] std::filesystem::path castStatePath() {
    return std::filesystem::path(FileUtils::stateDir()) / "phone-cast.json";
  }

  [[nodiscard]] bool castToolsReady() {
    return process::commandExists("scrcpy") && process::commandExists("adb");
  }

  [[nodiscard]] bool airplayToolsReady() { return process::commandExists("uxplay"); }

  // scripts/ sits beside assets/ both in the source tree and under <datadir>/noctalia.
  [[nodiscard]] std::filesystem::path phoneScriptPath(std::string_view name) {
    return paths::assetsRoot().parent_path() / "scripts" / std::filesystem::path(std::string(name));
  }

  // One JSON line to the Helix Phone control socket, one JSON line back. Blocks up to the
  // socket timeout (and the script answers status with a BlueZ round-trip), so callers on
  // the main thread go through pollPhonePointer(), which runs this on a worker.
  [[nodiscard]] std::optional<nlohmann::json> phonePointerRequest(const nlohmann::json& request) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }
    struct FdGuard {
      int fd;
      ~FdGuard() { ::close(fd); }
    } guard{fd};
    const timeval timeout{.tv_sec = 0, .tv_usec = 250'000};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = phonePointerSocketPath().string();
    if (path.size() >= sizeof(address.sun_path)) {
      return std::nullopt;
    }
    std::copy(path.begin(), path.end(), address.sun_path);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      return std::nullopt;
    }
    const std::string line = request.dump() + "\n";
    if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(line.size())) {
      return std::nullopt;
    }
    std::string reply;
    char buffer[512];
    while (reply.find('\n') == std::string::npos) {
      const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
      if (got <= 0) {
        return std::nullopt;
      }
      reply.append(buffer, static_cast<std::size_t>(got));
      if (reply.size() > 16 * 1024) {
        return std::nullopt;
      }
    }
    const auto parsed = nlohmann::json::parse(reply, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return std::nullopt;
    }
    return parsed;
  }

  void ensurePhonePointer() {
    if (phonePointerRequest({{"type", "status"}}).has_value()) {
      return;
    }
    const auto running = process::runSync({"pgrep", "-f", "helix-phone-hid.py"});
    if (running.exitCode == 0) {
      return;
    }
    const auto script = phoneScriptPath("helix-phone-hid.py");
    if (!std::filesystem::exists(script)) {
      kLog.warn("phone pointer script missing: {}", script.string());
      return;
    }
    if (!process::launchDetachedTracked({"python3", script.string()})) {
      kLog.warn("could not advertise Helix Phone over Bluetooth");
    }
  }

  [[nodiscard]] std::string labelFromPackage(std::string_view package) {
    const auto pos = package.rfind('.');
    std::string tail = pos == std::string_view::npos ? std::string(package) : std::string(package.substr(pos + 1));
    if (!tail.empty()) {
      tail[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tail[0])));
    }
    return tail;
  }

  [[nodiscard]] std::optional<std::string> parseAdbSerial(std::string_view output) {
    std::string first;
    for (std::size_t start = 0; start <= output.size();) {
      const std::size_t end = output.find('\n', start);
      const std::string_view line =
          end == std::string_view::npos ? output.substr(start) : output.substr(start, end - start);
      start = end == std::string_view::npos ? output.size() + 1 : end + 1;
      const auto tab = line.find('\t');
      if (tab == std::string_view::npos) {
        continue;
      }
      const std::string serial = StringUtils::trim(line.substr(0, tab));
      const std::string state = StringUtils::trim(line.substr(tab + 1));
      if (serial.empty() || serial == "List of devices attached") {
        continue;
      }
      if (state != "device") {
        continue;
      }
      if (first.empty()) {
        first = serial;
      }
    }
    return first.empty() ? std::nullopt : std::optional<std::string>{std::move(first)};
  }

  [[nodiscard]] std::vector<PhoneCastApp> parseCastApps(std::string_view output) {
    std::vector<PhoneCastApp> apps;
    for (std::size_t start = 0; start <= output.size();) {
      const std::size_t end = output.find('\n', start);
      const std::string_view line =
          end == std::string_view::npos ? output.substr(start) : output.substr(start, end - start);
      start = end == std::string_view::npos ? output.size() + 1 : end + 1;
      constexpr std::string_view kPrefix = "package:";
      if (!line.starts_with(kPrefix)) {
        continue;
      }
      std::string_view rest = line.substr(kPrefix.size());
      PhoneCastApp app;
      const auto eq = rest.find('=');
      if (eq != std::string_view::npos) {
        app.package = StringUtils::trim(rest.substr(0, eq));
        app.label = StringUtils::trim(rest.substr(eq + 1));
      } else {
        app.package = StringUtils::trim(rest);
        app.label = labelFromPackage(app.package);
      }
      if (app.package.empty()) {
        continue;
      }
      if (app.label.empty()) {
        app.label = labelFromPackage(app.package);
      }
      apps.push_back(std::move(app));
    }
    std::ranges::sort(apps, {}, &PhoneCastApp::label);
    return apps;
  }

  [[nodiscard]] std::vector<std::string> adbBaseArgs(const std::string& serial) {
    return serial.empty() ? std::vector<std::string>{"adb"} : std::vector<std::string>{"adb", "-s", serial};
  }

  [[nodiscard]] std::vector<PhoneCastApp> listCastApps(const std::string& serial) {
    auto args = adbBaseArgs(serial);
    args.push_back("shell");
    args.push_back("pm");
    args.push_back("list");
    args.push_back("packages");
    args.push_back("-3");
    args.push_back("--show-labels");
    const process::RunResult result = process::runSyncWithTimeout(args, std::chrono::seconds{20});
    if (result && !result.out.empty()) {
      auto apps = parseCastApps(result.out);
      if (!apps.empty()) {
        return apps;
      }
    }
    args.pop_back();
    const process::RunResult fallback = process::runSyncWithTimeout(args, std::chrono::seconds{20});
    if (!fallback) {
      return {};
    }
    return parseCastApps(fallback.out);
  }

  [[nodiscard]] std::string normalizeAdbHost(std::string_view host) {
    std::string out = StringUtils::trim(host);
    if (out.empty()) {
      return {};
    }
    if (out.find(':') == std::string::npos) {
      out += ":5555";
    }
    return out;
  }

  [[nodiscard]] std::optional<std::string> resolveAdbSerial(const std::vector<std::string>& hosts) {
    const process::RunResult devices = process::runSyncWithTimeout({"adb", "devices"}, std::chrono::seconds{8});
    if (auto serial = parseAdbSerial(devices.out)) {
      return serial;
    }
    for (const auto& rawHost : hosts) {
      const std::string host = normalizeAdbHost(rawHost);
      if (host.empty()) {
        continue;
      }
      (void)process::runSyncWithTimeout({"adb", "connect", host}, std::chrono::seconds{12});
      const process::RunResult retry = process::runSyncWithTimeout({"adb", "devices"}, std::chrono::seconds{8});
      if (auto serial = parseAdbSerial(retry.out)) {
        return serial;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<PhoneCastApp> prioritizeRecentApps(
      std::vector<PhoneCastApp> apps, const std::vector<PhoneCastApp>& recent
  ) {
    if (recent.empty() || apps.empty()) {
      return apps;
    }
    std::vector<PhoneCastApp> ordered;
    ordered.reserve(apps.size());
    for (const auto& recentApp : recent) {
      const auto it = std::ranges::find(apps, recentApp.package, &PhoneCastApp::package);
      if (it == apps.end()) {
        continue;
      }
      PhoneCastApp moved = *it;
      if (moved.label.empty()) {
        moved.label = recentApp.label;
      }
      ordered.push_back(std::move(moved));
      apps.erase(it);
    }
    ordered.insert(ordered.end(), std::make_move_iterator(apps.begin()), std::make_move_iterator(apps.end()));
    return ordered;
  }

  [[nodiscard]] std::string phoneMirrorWindowTitle(std::string_view label = {}) {
    using compositors::niri::kPhoneMirrorWindowTitle;
    if (label.empty() || label == kPhoneMirrorWindowTitle) {
      return std::string(kPhoneMirrorWindowTitle);
    }
    return std::string(label) + " · " + std::string(kPhoneMirrorWindowTitle);
  }

  void ensurePhoneMirrorWindowRules() {
    const auto result = compositors::niri::ensurePhoneCastWindowRules();
    if (!result.ok && !result.error.empty()) {
      kLog.warn("phone cast window rules: {}", result.error);
    }
  }

  [[nodiscard]] std::vector<std::string> scrcpyArgs(
      const std::string& serial, std::string windowTitle, bool audio, std::initializer_list<const char*> extra
  ) {
    using compositors::niri::kPhoneMirrorHeight;
    using compositors::niri::kPhoneMirrorWidth;
    ensurePhoneMirrorWindowRules();
    std::vector<std::string> args{"scrcpy"};
    if (!serial.empty()) {
      args.push_back("-s");
      args.push_back(serial);
    }
    for (const char* flag : extra) {
      args.emplace_back(flag);
    }
    // scrcpy forwards audio by default on Android 11+ and quietly carries on without it
    // on older phones; --no-audio is the explicit opt-out.
    if (!audio) {
      args.emplace_back("--no-audio");
    }
    args.push_back(std::format("--window-width={}", kPhoneMirrorWidth));
    args.push_back(std::format("--window-height={}", kPhoneMirrorHeight));
    args.push_back("--window-borderless");
    args.push_back("--always-on-top");
    args.push_back("--background-color=#000000");
    if (!windowTitle.empty()) {
      args.push_back("--window-title=" + phoneMirrorWindowTitle(windowTitle));
    } else {
      args.push_back("--window-title=" + phoneMirrorWindowTitle());
    }
    return args;
  }

  [[nodiscard]] std::filesystem::path castIconCacheDir(const std::string& deviceId) {
    return std::filesystem::path(FileUtils::stateDir()) / "phone-cast-icons" / deviceId;
  }

  [[nodiscard]] std::filesystem::path castIconCachePath(const std::string& deviceId, const std::string& package) {
    return castIconCacheDir(deviceId) / (package + ".png");
  }

  [[nodiscard]] int iconDensityScore(std::string_view entry) {
    const std::string lower = StringUtils::toLower(std::string(entry));
    if (lower.contains("xxxhdpi") || lower.contains("-640")) {
      return 6;
    }
    if (lower.contains("xxhdpi") || lower.contains("-480")) {
      return 5;
    }
    if (lower.contains("xhdpi") || lower.contains("-320")) {
      return 4;
    }
    if (lower.contains("hdpi") || lower.contains("-240")) {
      return 3;
    }
    if (lower.contains("mdpi") || lower.contains("-160")) {
      return 2;
    }
    if (lower.contains("anydpi") || lower.contains("nodpi")) {
      return 1;
    }
    return 0;
  }

  [[nodiscard]] bool looksLikeLauncherIcon(std::string_view entry) {
    const std::string lower = StringUtils::toLower(std::string(entry));
    if (!(lower.ends_with(".png") || lower.ends_with(".webp"))) {
      return false;
    }
    if (!(lower.contains("mipmap") || lower.contains("drawable"))) {
      return false;
    }
    return lower.contains("ic_launcher") || lower.contains("ic_app") || lower.contains("launcher")
        || lower.contains("app_icon") || lower.contains("/icon.") || lower.contains("_icon.");
  }

  [[nodiscard]] std::optional<std::string> pickApkIconEntry(std::string_view listing) {
    std::string best;
    int bestScore = -1;
    for (std::size_t start = 0; start <= listing.size();) {
      const std::size_t end = listing.find('\n', start);
      const std::string_view line =
          end == std::string_view::npos ? listing.substr(start) : listing.substr(start, end - start);
      start = end == std::string_view::npos ? listing.size() + 1 : end + 1;
      // unzip -l lines end with the entry path after a space-padded size/date block.
      std::string path;
      const auto slash = line.find(" res/");
      const auto slashAlt = line.find(" assets/");
      if (slash != std::string_view::npos) {
        path = StringUtils::trim(line.substr(slash + 1));
      } else if (slashAlt != std::string_view::npos) {
        path = StringUtils::trim(line.substr(slashAlt + 1));
      } else {
        const auto lastSpace = line.find_last_of(' ');
        if (lastSpace == std::string_view::npos) {
          continue;
        }
        path = StringUtils::trim(line.substr(lastSpace + 1));
      }
      if (!looksLikeLauncherIcon(path)) {
        continue;
      }
      int score = iconDensityScore(path);
      if (path.ends_with(".png") || path.ends_with(".PNG")) {
        score += 10;
      }
      if (score > bestScore) {
        bestScore = score;
        best = std::move(path);
      }
    }
    return best.empty() ? std::nullopt : std::optional<std::string>{std::move(best)};
  }

  [[nodiscard]] std::optional<std::string> apkPathForPackage(const std::string& serial, const std::string& package) {
    auto args = adbBaseArgs(serial);
    args.push_back("shell");
    args.push_back("pm");
    args.push_back("path");
    args.push_back(package);
    const process::RunResult result = process::runSyncWithTimeout(args, std::chrono::seconds{8});
    if (!result) {
      return std::nullopt;
    }
    for (std::size_t start = 0; start <= result.out.size();) {
      const std::size_t end = result.out.find('\n', start);
      const std::string_view line =
          end == std::string_view::npos ? std::string_view{result.out}.substr(start)
                                        : std::string_view{result.out}.substr(start, end - start);
      start = end == std::string_view::npos ? result.out.size() + 1 : end + 1;
      constexpr std::string_view kPrefix = "package:";
      if (!line.starts_with(kPrefix)) {
        continue;
      }
      std::string path = StringUtils::trim(line.substr(kPrefix.size()));
      if (!path.empty() && path.ends_with("base.apk")) {
        return path;
      }
      if (!path.empty()) {
        return path;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool extractApkIconTo(const std::filesystem::path& apkPath, const std::filesystem::path& dest) {
    if (!process::commandExists("unzip")) {
      return false;
    }
    const process::RunResult listing =
        process::runSyncWithTimeout({"unzip", "-l", apkPath.string()}, std::chrono::seconds{8});
    if (!listing) {
      return false;
    }
    const auto entry = pickApkIconEntry(listing.out);
    if (!entry) {
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    const auto tmp = dest.string() + ".part";
    const process::RunResult extract = process::runSyncWithTimeout(
        {"/bin/sh", "-c", "unzip -p \"$1\" \"$2\" > \"$3\"", "extract-apk-icon", apkPath.string(), *entry, tmp},
        std::chrono::seconds{12}
    );
    if (!extract || !std::filesystem::exists(tmp, ec) || std::filesystem::file_size(tmp, ec) == 0) {
      std::filesystem::remove(tmp, ec);
      return false;
    }
    std::filesystem::rename(tmp, dest, ec);
    return !ec;
  }

  [[nodiscard]] bool fetchCastAppIcon(
      const std::string& serial, const std::string& deviceId, const std::string& package
  ) {
    const auto dest = castIconCachePath(deviceId, package);
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && std::filesystem::file_size(dest, ec) > 0) {
      return true;
    }
    const auto remote = apkPathForPackage(serial, package);
    if (!remote) {
      return false;
    }
    const auto tmpDir = std::filesystem::temp_directory_path() / "helix-cast-apk";
    std::filesystem::create_directories(tmpDir, ec);
    const auto apk = tmpDir / (package + ".apk");
    auto pull = adbBaseArgs(serial);
    pull.push_back("pull");
    pull.push_back(*remote);
    pull.push_back(apk.string());
    const process::RunResult pulled = process::runSyncWithTimeout(pull, std::chrono::seconds{45});
    const bool ok = pulled && extractApkIconTo(apk, dest);
    std::filesystem::remove(apk, ec);
    return ok;
  }

  [[nodiscard]] std::vector<PhoneCastApp> applyCachedIcons(
      std::vector<PhoneCastApp> apps, const std::string& deviceId
  ) {
    for (auto& app : apps) {
      const auto path = castIconCachePath(deviceId, app.package);
      std::error_code ec;
      if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) {
        app.iconPath = path.string();
      }
    }
    return apps;
  }

  [[nodiscard]] int hexValue(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  [[nodiscard]] std::string percentDecode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '%' && i + 2 < in.size()) {
        const int hi = hexValue(in[i + 1]);
        const int lo = hexValue(in[i + 2]);
        if (hi >= 0 && lo >= 0) {
          out.push_back(static_cast<char>((hi << 4) | lo));
          i += 2;
          continue;
        }
      }
      out.push_back(in[i]);
    }
    return out;
  }

  [[nodiscard]] std::string fileUrlToPath(std::string_view url) {
    constexpr std::string_view kPrefix = "file://";
    if (url.size() >= kPrefix.size() && url.substr(0, kPrefix.size()) == kPrefix) {
      std::string path = percentDecode(url.substr(kPrefix.size()));
      if (path.starts_with("localhost/")) {
        path.erase(0, 9);
      }
      return path;
    }
    return std::string(url);
  }

  [[nodiscard]] std::string continueTitle(std::string_view url) {
    const std::string path = fileUrlToPath(url);
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < path.size()) {
      return path.substr(slash + 1);
    }
    return std::string(url);
  }

  [[nodiscard]] bool urlLooksFile(std::string_view url) {
    return url.starts_with("file://") || (!url.empty() && url[0] == '/');
  }

  [[nodiscard]] std::string extractAuthCode(std::string_view text) {
    std::string best;
    std::string run;
    const auto consider = [&]() {
      if (run.size() >= 4 && run.size() <= 10) {
        const bool runIsSix = run.size() == 6;
        const bool bestIsSix = best.size() == 6;
        if (best.empty() || (runIsSix && !bestIsSix) || (!bestIsSix && !runIsSix && run.size() > best.size())) {
          best = run;
        }
      }
      run.clear();
    };
    for (const unsigned char c : text) {
      if (c >= '0' && c <= '9') {
        run.push_back(static_cast<char>(c));
      } else {
        consider();
      }
    }
    consider();
    return best;
  }

  [[nodiscard]] std::string notificationObjectPath(const std::string& deviceId, const std::string& publicId) {
    return pluginObjectPath(deviceId, "notifications") + "/" + publicId;
  }

} // namespace

struct PhoneConnectService::Impl {
  PhoneConnectService& self;
  SessionBus& bus;
  std::unique_ptr<sdbus::IProxy> dbus;
  sdbus::Slot nameWatch;
  std::unique_ptr<sdbus::IProxy> daemon;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> deviceProxies;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> batteryProxies;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> shareProxies;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> sftpProxies;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> notificationProxies;
  std::vector<std::unique_ptr<sdbus::IProxy>> inflight;
  std::shared_ptr<void> lifetime = std::make_shared<int>(0);
  bool discoveryHeld = false;
  bool wantDiscovery = false;
  bool devicesTwoArg = true;
  std::uint64_t listGeneration = 0;

  explicit Impl(PhoneConnectService& s, SessionBus& b) : self(s), bus(b) {}

  [[nodiscard]] std::weak_ptr<void> token() const { return lifetime; }

  PhoneDevice* findDevice(const std::string& id) { return self.findDevice(id); }

  std::unique_ptr<sdbus::IProxy> makeProxy(const std::string& path) {
    return sdbus::createProxy(bus.connection(), kKdeConnectBusName, sdbus::ObjectPath{path});
  }

  sdbus::IProxy* takeInflight(const std::string& path) {
    try {
      inflight.push_back(makeProxy(path));
      return inflight.back().get();
    } catch (const sdbus::Error& e) {
      kLog.warn("proxy create failed {}: {}", path, e.what());
      return nullptr;
    }
  }

  void fireVoid(const std::string& path, const char* interface, const char* method) {
    auto* proxy = takeInflight(path);
    if (proxy == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      proxy->callMethodAsync(method).onInterface(interface).uponReplyInvoke(
          [alive, path, method](std::optional<sdbus::Error> err) {
            if (alive.expired() || !err.has_value()) {
              return;
            }
            kLog.warn("{} {} failed: {}", method, path, err->what());
          }
      );
    } catch (const sdbus::Error& e) {
      kLog.warn("{} dispatch failed {}: {}", method, path, e.what());
    }
  }

  void fireString(const std::string& path, const char* interface, const char* method, std::string arg) {
    auto* proxy = takeInflight(path);
    if (proxy == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      proxy->callMethodAsync(method)
          .onInterface(interface)
          .withArguments(std::move(arg))
          .uponReplyInvoke([alive, path, method](std::optional<sdbus::Error> err) {
            if (alive.expired() || !err.has_value()) {
              return;
            }
            kLog.warn("{} {} failed: {}", method, path, err->what());
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("{} dispatch failed {}: {}", method, path, e.what());
    }
  }

  void ensureDeviceProxy(const std::string& id) {
    if (deviceProxies.contains(id)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(deviceObjectPath(id));
    } catch (const sdbus::Error& e) {
      kLog.debug("device proxy failed {}: {}", id, e.what());
      return;
    }
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, id](
                  const std::string& interfaceName, const InterfaceProps& changed,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName != kDeviceInterface) {
            return;
          }
          auto* device = findDevice(id);
          if (device == nullptr) {
            self.scheduleRefresh();
            return;
          }
          PhoneDevice updated = *device;
          mergeDeviceProps(changed, updated);
          if (updated != *device) {
            *device = std::move(updated);
            self.notifyChanged();
          }
        });
    deviceProxies.emplace(id, std::move(proxy));
  }

  void ensureBatteryProxy(const std::string& id) {
    if (batteryProxies.contains(id)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(pluginObjectPath(id, "battery"));
    } catch (const sdbus::Error& e) {
      kLog.debug("battery proxy failed {}: {}", id, e.what());
      return;
    }
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, id](
                  const std::string& interfaceName, const InterfaceProps& changed,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName != kBatteryInterface) {
            return;
          }
          auto* device = findDevice(id);
          if (device == nullptr) {
            return;
          }
          PhoneDevice updated = *device;
          mergeBatteryProps(changed, updated);
          if (updated != *device) {
            *device = std::move(updated);
            self.notifyChanged();
          }
        });
    proxy->uponSignal("refreshed")
        .onInterface(kBatteryInterface)
        .call([this, id](bool charging, std::int32_t charge) {
          auto* device = findDevice(id);
          if (device == nullptr) {
            return;
          }
          device->charging = charging;
          device->charge = charge;
          device->hasBattery = charge >= 0;
          self.notifyChanged();
        });
    batteryProxies.emplace(id, std::move(proxy));
  }

  void loadBattery(const std::string& id) {
    ensureBatteryProxy(id);
    auto it = batteryProxies.find(id);
    if (it == batteryProxies.end() || it->second == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(std::string{kBatteryInterface})
          .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, InterfaceProps props) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              return;
            }
            auto* device = findDevice(id);
            if (device == nullptr) {
              return;
            }
            PhoneDevice updated = *device;
            mergeBatteryProps(props, updated);
            if (updated != *device) {
              *device = std::move(updated);
              self.notifyChanged();
            }
          });
    } catch (const sdbus::Error&) {
    }
  }

  void applyDeviceList(std::vector<std::string> ids) {
    std::vector<PhoneDevice> next;
    next.reserve(ids.size());
    std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> keepDevices;
    std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> keepBatteries;
    std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> keepShare;
    std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> keepSftp;
    std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> keepNotifications;
    for (auto& id : ids) {
      if (id.empty()) {
        continue;
      }
      PhoneDevice device;
      if (auto* existing = findDevice(id)) {
        device = *existing;
      }
      device.id = id;
      next.push_back(std::move(device));
      if (auto it = deviceProxies.find(id); it != deviceProxies.end()) {
        keepDevices.emplace(id, std::move(it->second));
      }
      if (auto it = batteryProxies.find(id); it != batteryProxies.end()) {
        keepBatteries.emplace(id, std::move(it->second));
      }
      if (auto it = shareProxies.find(id); it != shareProxies.end()) {
        keepShare.emplace(id, std::move(it->second));
      }
      if (auto it = sftpProxies.find(id); it != sftpProxies.end()) {
        keepSftp.emplace(id, std::move(it->second));
      }
      if (auto it = notificationProxies.find(id); it != notificationProxies.end()) {
        keepNotifications.emplace(id, std::move(it->second));
      }
    }
    deviceProxies = std::move(keepDevices);
    batteryProxies = std::move(keepBatteries);
    shareProxies = std::move(keepShare);
    sftpProxies = std::move(keepSftp);
    notificationProxies = std::move(keepNotifications);
    std::erase_if(self.m_notifications, [&](const PhoneNotification& item) {
      return std::ranges::none_of(next, [&](const PhoneDevice& device) { return device.id == item.deviceId; });
    });
    self.m_snapshot.devices = std::move(next);
    self.m_snapshot.error.clear();
    self.notifyChanged();

    for (const auto& device : self.m_snapshot.devices) {
      ensureDeviceProxy(device.id);
      loadDevice(device.id);
    }
  }

  void loadDevice(const std::string& id) {
    ensureDeviceProxy(id);
    auto it = deviceProxies.find(id);
    if (it == deviceProxies.end() || it->second == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(std::string{kDeviceInterface})
          .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, InterfaceProps props) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.debug("device GetAll failed {}: {}", id, err->what());
              return;
            }
            PhoneDevice* device = findDevice(id);
            if (device == nullptr) {
              PhoneDevice created;
              device = &self.m_snapshot.devices.emplace_back(std::move(created));
              device->id = id;
            }
            PhoneDevice updated = *device;
            mergeDeviceProps(props, updated);
            if (updated.id.empty()) {
              updated.id = id;
            }
            *device = std::move(updated);
            self.notifyChanged();
            if (device->paired) {
              loadBattery(id);
              probePlugins(id);
              ensureShareProxy(id);
              ensureNotificationsProxy(id);
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("device GetAll dispatch failed {}: {}", id, e.what());
    }
  }

  void requestDeviceList() {
    if (daemon == nullptr) {
      return;
    }
    const std::uint64_t generation = ++listGeneration;
    const std::weak_ptr<void> alive = token();
    auto onList = [this, alive, generation](std::optional<sdbus::Error> err, std::vector<std::string> ids) {
      if (alive.expired() || generation != listGeneration) {
        return;
      }
      if (err.has_value()) {
        if (devicesTwoArg && isUnknownMethod(*err)) {
          devicesTwoArg = false;
          requestDeviceList();
          return;
        }
        kLog.warn("devices() failed: {}", err->what());
        self.m_snapshot.error = i18n::tr("control-center.phone.status-failed");
        self.notifyChanged();
        return;
      }
      applyDeviceList(std::move(ids));
    };

    try {
      if (devicesTwoArg) {
        daemon->callMethodAsync("devices")
            .onInterface(kDaemonInterface)
            .withArguments(false, false)
            .uponReplyInvoke(onList);
      } else {
        daemon->callMethodAsync("devices").onInterface(kDaemonInterface).withArguments(false).uponReplyInvoke(onList);
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("devices() dispatch failed: {}", e.what());
    }
  }

  void loadAnnouncedName() {
    if (daemon == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      daemon->callMethodAsync("announcedName")
          .onInterface(kDaemonInterface)
          .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, std::string name) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              return;
            }
            if (self.m_snapshot.announcedName != name) {
              self.m_snapshot.announcedName = std::move(name);
              self.notifyChanged();
            }
          });
    } catch (const sdbus::Error&) {
    }
  }

  void probePlugin(const std::string& id, const char* plugin, bool PhoneDevice::*field) {
    auto it = deviceProxies.find(id);
    if (it == deviceProxies.end() || it->second == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("hasPlugin")
          .onInterface(kDeviceInterface)
          .withArguments(std::string{plugin})
          .uponReplyInvoke([this, alive, id, field](std::optional<sdbus::Error> err, bool present) {
            if (alive.expired() || err.has_value()) {
              return;
            }
            auto* device = findDevice(id);
            if (device == nullptr) {
              return;
            }
            if (device->*field == present) {
              return;
            }
            device->*field = present;
            self.notifyChanged();
            if (present && field == &PhoneDevice::sharePlugin) {
              ensureShareProxy(id);
            }
            if (present && field == &PhoneDevice::notificationsPlugin) {
              ensureNotificationsProxy(id);
            }
          });
    } catch (const sdbus::Error&) {
    }
  }

  void probePlugins(const std::string& id) {
    probePlugin(id, kSharePlugin, &PhoneDevice::sharePlugin);
    probePlugin(id, kSftpPlugin, &PhoneDevice::sftpPlugin);
    probePlugin(id, kNotificationsPlugin, &PhoneDevice::notificationsPlugin);
  }

  void ensureShareProxy(const std::string& id) {
    if (shareProxies.contains(id)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(pluginObjectPath(id, "share"));
    } catch (const sdbus::Error& e) {
      kLog.debug("share proxy failed {}: {}", id, e.what());
      return;
    }
    proxy->uponSignal("shareReceived")
        .onInterface(kShareInterface)
        .call([this, id](const std::string& url) { self.recordShare(id, url); });
    shareProxies.emplace(id, std::move(proxy));
  }

  void upsertNotification(PhoneNotification item) {
    auto it = std::ranges::find_if(self.m_notifications, [&](const PhoneNotification& existing) {
      return existing.deviceId == item.deviceId && existing.publicId == item.publicId;
    });
    if (it != self.m_notifications.end()) {
      *it = std::move(item);
    } else {
      self.m_notifications.insert(self.m_notifications.begin(), std::move(item));
    }
    if (self.m_notifications.size() > 48) {
      self.m_notifications.resize(48);
    }
    self.notifyChanged();
  }

  void loadNotification(const std::string& deviceId, const std::string& publicId) {
    if (deviceId.empty() || publicId.empty()) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(notificationObjectPath(deviceId, publicId));
    } catch (const sdbus::Error& e) {
      kLog.debug("notification proxy failed {} {}: {}", deviceId, publicId, e.what());
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      proxy->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(std::string{kNotificationItemInterface})
          .uponReplyInvoke([this, alive, deviceId, publicId](std::optional<sdbus::Error> err, InterfaceProps props) {
            if (alive.expired() || err.has_value()) {
              return;
            }
            PhoneNotification item;
            item.deviceId = deviceId;
            item.publicId = publicId;
            if (auto it = props.find("appName"); it != props.end()) {
              if (auto v = variantGet<std::string>(it->second)) {
                item.appName = std::move(*v);
              }
            }
            if (auto it = props.find("title"); it != props.end()) {
              if (auto v = variantGet<std::string>(it->second)) {
                item.title = std::move(*v);
              }
            }
            if (auto it = props.find("text"); it != props.end()) {
              if (auto v = variantGet<std::string>(it->second)) {
                item.text = std::move(*v);
              }
            }
            if (auto it = props.find("ticker"); it != props.end()) {
              if (auto v = variantGet<std::string>(it->second)) {
                item.ticker = std::move(*v);
              }
            }
            if (auto it = props.find("dismissable"); it != props.end()) {
              if (auto v = variantGet<bool>(it->second)) {
                item.dismissable = *v;
              }
            }
            item.smsCode = extractAuthCode(item.title + " " + item.text + " " + item.ticker);
            upsertNotification(std::move(item));
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("notification GetAll failed {} {}: {}", deviceId, publicId, e.what());
    }
    inflight.push_back(std::move(proxy));
  }

  void refreshNotifications(const std::string& id) {
    ensureNotificationsProxy(id);
    auto it = notificationProxies.find(id);
    if (it == notificationProxies.end() || it->second == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("activeNotifications")
          .onInterface(kNotificationsInterface)
          .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, std::vector<std::string> ids) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.debug("activeNotifications failed {}: {}", id, err->what());
              return;
            }
            std::erase_if(self.m_notifications, [&](const PhoneNotification& item) {
              return item.deviceId == id && std::ranges::find(ids, item.publicId) == ids.end();
            });
            for (const auto& publicId : ids) {
              loadNotification(id, publicId);
            }
            self.notifyChanged();
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("activeNotifications dispatch failed {}: {}", id, e.what());
    }
  }

  void ensureNotificationsProxy(const std::string& id) {
    if (notificationProxies.contains(id)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(pluginObjectPath(id, "notifications"));
    } catch (const sdbus::Error& e) {
      kLog.debug("notifications proxy failed {}: {}", id, e.what());
      return;
    }
    proxy->uponSignal("notificationPosted")
        .onInterface(kNotificationsInterface)
        .call([this, id](const std::string& publicId) { loadNotification(id, publicId); });
    proxy->uponSignal("notificationUpdated")
        .onInterface(kNotificationsInterface)
        .call([this, id](const std::string& publicId) { loadNotification(id, publicId); });
    proxy->uponSignal("notificationRemoved")
        .onInterface(kNotificationsInterface)
        .call([this, id](const std::string& publicId) {
          const auto before = self.m_notifications.size();
          std::erase_if(self.m_notifications, [&](const PhoneNotification& item) {
            return item.deviceId == id && item.publicId == publicId;
          });
          if (self.m_notifications.size() != before) {
            self.notifyChanged();
          }
        });
    proxy->uponSignal("allNotificationsRemoved").onInterface(kNotificationsInterface).call([this, id]() {
      const auto before = self.m_notifications.size();
      std::erase_if(self.m_notifications, [&](const PhoneNotification& item) { return item.deviceId == id; });
      if (self.m_notifications.size() != before) {
        self.notifyChanged();
      }
    });
    notificationProxies.emplace(id, std::move(proxy));
    refreshNotifications(id);
  }

  void ensureSftpProxy(const std::string& id) {
    if (sftpProxies.contains(id)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = makeProxy(pluginObjectPath(id, "sftp"));
    } catch (const sdbus::Error& e) {
      kLog.debug("sftp proxy failed {}: {}", id, e.what());
      return;
    }
    proxy->uponSignal("mounted").onInterface(kSftpInterface).call([this, id]() {
      if (self.m_files.deviceId == id) {
        self.m_files.mounted = true;
        self.m_files.mounting = false;
        self.m_files.error.clear();
        loadDirectories(id);
      }
    });
    proxy->uponSignal("unmounted").onInterface(kSftpInterface).call([this, id]() {
      if (self.m_files.deviceId == id) {
        self.m_files.mounted = false;
        self.m_files.mounting = false;
        self.m_files.directories.clear();
        self.notifyChanged();
      }
    });
    sftpProxies.emplace(id, std::move(proxy));
  }

  void loadDirectories(const std::string& id) {
    ensureSftpProxy(id);
    auto it = sftpProxies.find(id);
    if (it == sftpProxies.end() || it->second == nullptr) {
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("getDirectories")
          .onInterface(kSftpInterface)
          .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, std::map<std::string, sdbus::Variant> dirs) {
            if (alive.expired()) {
              return;
            }
            if (self.m_files.deviceId != id) {
              return;
            }
            self.m_files.mounting = false;
            if (err.has_value()) {
              self.m_files.error = err->what();
              self.notifyChanged();
              return;
            }
            self.m_files.mounted = true;
            self.m_files.error.clear();
            self.m_files.directories.clear();
            for (auto& [label, value] : dirs) {
              if (auto path = variantGet<std::string>(value); path && !path->empty()) {
                self.m_files.directories.emplace_back(label, fileUrlToPath(*path));
              }
            }
            self.notifyChanged();
          });
    } catch (const sdbus::Error& e) {
      self.m_files.mounting = false;
      self.m_files.error = e.what();
      self.notifyChanged();
    }
  }

  void mountFiles(const std::string& id) {
    auto* device = findDevice(id);
    if (device == nullptr || !device->reachable) {
      self.m_files = {.deviceId = id, .error = i18n::tr("control-center.phone.files-offline")};
      self.notifyChanged();
      return;
    }
    self.m_files = {.deviceId = id, .supported = true, .mounting = true};
    self.notifyChanged();
    ensureSftpProxy(id);
    auto it = sftpProxies.find(id);
    if (it == sftpProxies.end() || it->second == nullptr) {
      self.m_files.mounting = false;
      self.m_files.error = i18n::tr("control-center.phone.files-unsupported");
      self.notifyChanged();
      return;
    }
    const std::weak_ptr<void> alive = token();
    try {
      it->second->callMethodAsync("mountAndWait")
          .onInterface(kSftpInterface)
          .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, bool ok) {
            if (alive.expired() || self.m_files.deviceId != id) {
              return;
            }
            if (err.has_value() || !ok) {
              self.m_files.mounting = false;
              self.m_files.mounted = false;
              if (err.has_value()) {
                self.m_files.error = err->what();
              } else {
                self.m_files.error = i18n::tr("control-center.phone.files-mount-failed");
              }
              self.notifyChanged();
              return;
            }
            loadDirectories(id);
          });
    } catch (const sdbus::Error& e) {
      self.m_files.mounting = false;
      self.m_files.error = e.what();
      self.notifyChanged();
    }
  }

  void applyDiscovery(bool enabled) {
    wantDiscovery = enabled;
    if (daemon == nullptr) {
      discoveryHeld = false;
      return;
    }
    const char* method = enabled ? "acquireDiscoveryMode" : "releaseDiscoveryMode";
    try {
      const std::weak_ptr<void> alive = token();
      daemon->callMethodAsync(method)
          .onInterface(kDaemonInterface)
          .withArguments(std::string{kDiscoveryToken})
          .uponReplyInvoke([alive, method](std::optional<sdbus::Error> err) {
            if (alive.expired() || !err.has_value()) {
              return;
            }
            kLog.debug("{} failed: {}", method, err->what());
          });
      discoveryHeld = enabled;
    } catch (const sdbus::Error& e) {
      kLog.debug("{} dispatch failed: {}", method, e.what());
    }
    if (enabled) {
      try {
        daemon->callMethodAsync("forceOnNetworkChange")
            .onInterface(kDaemonInterface)
            .uponReplyInvoke([](std::optional<sdbus::Error>) {});
      } catch (const sdbus::Error&) {
      }
    }
  }

  void watchDaemonSignals() {
    if (daemon == nullptr) {
      return;
    }
    daemon->uponSignal("deviceAdded")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& /*id*/) { self.scheduleRefresh(); });
    daemon->uponSignal("deviceRemoved")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& /*id*/) { self.scheduleRefresh(); });
    daemon->uponSignal("deviceListChanged").onInterface(kDaemonInterface).call([this]() { self.scheduleRefresh(); });
    daemon->uponSignal("deviceVisibilityChanged")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& /*id*/, bool /*visible*/) { self.scheduleRefresh(); });
    daemon->uponSignal("announcedNameChanged")
        .onInterface(kDaemonInterface)
        .call([this](const std::string& name) {
          self.m_snapshot.announcedName = name;
          self.notifyChanged();
        });
  }
};

PhoneConnectService::PhoneConnectService(SessionBus& bus)
    : m_bus(bus), m_impl(std::make_unique<Impl>(*this, bus)) {
  loadContinueHistory();
  loadCastState();
}

PhoneConnectService::~PhoneConnectService() {
  // Listeners (Control Center tabs) may already be gone; no change signals from here.
  m_destroying = true;
  stopCastMirror();
  if (m_impl != nullptr) {
    m_impl->applyDiscovery(false);
    m_impl->lifetime.reset();
  }
}

void PhoneConnectService::setChangeCallback(ChangeCallback callback) {
  m_legacyChangeConn.disconnect();
  if (callback) {
    m_legacyChangeConn = m_changed.connect(std::move(callback));
  }
}

void PhoneConnectService::start() {
  try {
    m_impl->dbus = sdbus::createProxy(m_bus.connection(), kDbusName, kDbusPath);
  } catch (const sdbus::Error& e) {
    kLog.warn("session dbus proxy failed: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.phone.missing");
    notifyChanged();
    return;
  }

  m_impl->nameWatch = dbus_watch::watchBusName(
      m_bus.connection(), "org.kde.kdeconnect",
      [this](const std::string& /*name*/, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (newOwner.empty()) {
          detachDaemon();
        } else {
          attachDaemon();
        }
      }
  );

  bool hasOwner = false;
  try {
    m_impl->dbus->callMethod("NameHasOwner")
        .onInterface(kDbusInterface)
        .withArguments(std::string{"org.kde.kdeconnect"})
        .storeResultsTo(hasOwner);
  } catch (const sdbus::Error& e) {
    kLog.debug("NameHasOwner failed: {}", e.what());
  }
  if (hasOwner) {
    attachDaemon();
  } else {
    m_snapshot.available = false;
    notifyChanged();
  }
  pollPhonePointer();
}

void PhoneConnectService::scheduleRefresh() {
  m_refreshTimer.stop();
  m_refreshTimer.start(kRefreshDebounce, [this]() { refresh(); });
}

void PhoneConnectService::refresh() {
  if (!m_snapshot.available) {
    bool hasOwner = false;
    if (m_impl->dbus != nullptr) {
      try {
        m_impl->dbus->callMethod("NameHasOwner")
            .onInterface(kDbusInterface)
            .withArguments(std::string{"org.kde.kdeconnect"})
            .storeResultsTo(hasOwner);
      } catch (const sdbus::Error&) {
      }
    }
    if (hasOwner) {
      attachDaemon();
    }
    return;
  }
  if (m_impl->daemon == nullptr) {
    attachDaemon();
    return;
  }
  m_impl->loadAnnouncedName();
  m_impl->requestDeviceList();
}

void PhoneConnectService::tryActivate() {
  if (m_snapshot.available || m_impl->dbus == nullptr) {
    return;
  }
  m_snapshot.activating = true;
  notifyChanged();
  const std::weak_ptr<void> alive = m_impl->token();
  try {
    m_impl->dbus->callMethodAsync("StartServiceByName")
        .onInterface(kDbusInterface)
        .withArguments(std::string{"org.kde.kdeconnect"}, std::uint32_t{0})
        .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, std::uint32_t /*result*/) {
          if (alive.expired()) {
            return;
          }
          m_snapshot.activating = false;
          if (err.has_value()) {
            kLog.info("kdeconnectd is not available: {}", err->what());
            m_snapshot.available = false;
            m_snapshot.error.clear();
            notifyChanged();
            return;
          }
          attachDaemon();
        });
  } catch (const sdbus::Error& e) {
    m_snapshot.activating = false;
    kLog.info("kdeconnectd start failed: {}", e.what());
    notifyChanged();
  }
}

void PhoneConnectService::setDiscovery(bool enabled) { m_impl->applyDiscovery(enabled); }

void PhoneConnectService::requestPair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "requestPairing");
}

void PhoneConnectService::acceptPair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "acceptPairing");
}

void PhoneConnectService::rejectPair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "rejectPairing");
}

void PhoneConnectService::unpair(const std::string& deviceId) {
  m_impl->fireVoid(deviceObjectPath(deviceId), kDeviceInterface, "unpair");
}

void PhoneConnectService::ping(const std::string& deviceId) {
  m_impl->fireVoid(pluginObjectPath(deviceId, "ping"), kPingInterface, "sendPing");
}

void PhoneConnectService::ring(const std::string& deviceId) {
  m_impl->fireVoid(pluginObjectPath(deviceId, "findmyphone"), kFindMyPhoneInterface, "ring");
}

void PhoneConnectService::shareUrl(const std::string& deviceId, std::string url) {
  if (url.empty()) {
    return;
  }
  m_impl->fireString(pluginObjectPath(deviceId, "share"), kShareInterface, "shareUrl", std::move(url));
}

void PhoneConnectService::shareText(const std::string& deviceId, std::string text) {
  if (text.empty()) {
    return;
  }
  m_impl->fireString(pluginObjectPath(deviceId, "share"), kShareInterface, "shareText", std::move(text));
}

void PhoneConnectService::sharePath(const std::string& deviceId, std::string path) {
  if (path.empty()) {
    return;
  }
  shareUrl(deviceId, fileUrlFromPath(path));
}

void PhoneConnectService::mountFiles(const std::string& deviceId) { m_impl->mountFiles(deviceId); }

void PhoneConnectService::refreshFiles(const std::string& deviceId) {
  if (m_files.mounted && m_files.deviceId == deviceId) {
    m_impl->loadDirectories(deviceId);
    return;
  }
  mountFiles(deviceId);
}

void PhoneConnectService::loadContinueHistory() {
  m_continueItems.clear();
  std::ifstream in(continueStatePath());
  if (!in) {
    return;
  }
  nlohmann::json json;
  try {
    in >> json;
  } catch (const nlohmann::json::exception&) {
    return;
  }
  if (!json.is_array()) {
    return;
  }
  for (const auto& row : json) {
    if (!row.is_object()) {
      continue;
    }
    PhoneContinueItem item;
    item.url = row.value("url", std::string{});
    if (item.url.empty()) {
      continue;
    }
    item.title = row.value("title", continueTitle(item.url));
    item.deviceId = row.value("deviceId", std::string{});
    item.deviceName = row.value("deviceName", std::string{});
    item.timestamp = row.value("ts", std::int64_t{0});
    item.isFile = row.value("file", urlLooksFile(item.url));
    item.isImage = row.value("image", DirectoryScanner::isImagePath(fileUrlToPath(item.url)));
    m_continueItems.push_back(std::move(item));
    if (m_continueItems.size() >= 24) {
      break;
    }
  }
}

void PhoneConnectService::saveContinueHistory() {
  nlohmann::json json = nlohmann::json::array();
  for (const auto& item : m_continueItems) {
    json.push_back({
        {"url", item.url},
        {"title", item.title},
        {"deviceId", item.deviceId},
        {"deviceName", item.deviceName},
        {"ts", item.timestamp},
        {"file", item.isFile},
        {"image", item.isImage},
    });
  }
  const auto path = continueStatePath();
  std::error_code ec;
  FileUtils::createPrivateDirectories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return;
  }
  out << json.dump(2);
}

void PhoneConnectService::recordShare(const std::string& deviceId, std::string url) {
  if (url.empty()) {
    return;
  }
  PhoneContinueItem item;
  item.url = std::move(url);
  item.title = continueTitle(item.url);
  item.deviceId = deviceId;
  if (const auto* device = findDevice(deviceId)) {
    item.deviceName = device->name;
  }
  item.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()
  )
                       .count();
  item.isFile = urlLooksFile(item.url);
  item.isImage = item.isFile && DirectoryScanner::isImagePath(fileUrlToPath(item.url));
  std::erase_if(m_continueItems, [&](const PhoneContinueItem& existing) { return existing.url == item.url; });
  m_continueItems.insert(m_continueItems.begin(), std::move(item));
  if (m_continueItems.size() > 24) {
    m_continueItems.resize(24);
  }
  saveContinueHistory();
  notifyChanged();
}

std::optional<std::filesystem::path> PhoneConnectService::pullFile(const std::string& sourcePath) {
  const std::filesystem::path source = FileUtils::expandUserPath(fileUrlToPath(sourcePath));
  std::error_code ec;
  if (source.empty() || !std::filesystem::is_regular_file(source, ec) || ec) {
    return std::nullopt;
  }
  auto destDir = FileUtils::defaultPicturesDirectory() / "Helix";
  if (!FileUtils::createPrivateDirectories(destDir, ec) && ec) {
    destDir = FileUtils::defaultPicturesDirectory();
    std::filesystem::create_directories(destDir, ec);
  }
  std::filesystem::path dest = destDir / source.filename();
  if (std::filesystem::exists(dest, ec)) {
    dest = destDir / (source.stem().string() + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
                      + source.extension().string());
  }
  std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    return std::nullopt;
  }
  return dest;
}

void PhoneConnectService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::phoneStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: phone-status takes no arguments\n";
    }
    if (!m_snapshot.available) {
      return m_snapshot.activating ? "starting\n" : "not-installed\n";
    }
    int reachable = 0;
    int paired = 0;
    for (const auto& device : m_snapshot.devices) {
      if (device.paired) {
        ++paired;
      }
      if (device.reachable) {
        ++reachable;
      }
    }
    return std::format(
        "running name={} paired={} reachable={}\n", m_snapshot.announcedName.empty() ? "-" : m_snapshot.announcedName,
        paired, reachable
    );
  });
  ipc.bind(noctalia::cli::msg::phonePing, [this](const std::string& args) -> std::string {
    const auto* device = resolveDevice(StringUtils::trim(args));
    if (device == nullptr) {
      return "error: no matching paired device\n";
    }
    ping(device->id);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::phoneRing, [this](const std::string& args) -> std::string {
    const auto* device = resolveDevice(StringUtils::trim(args));
    if (device == nullptr) {
      return "error: no matching paired device\n";
    }
    ring(device->id);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::phoneCast, [this](const std::string& args) -> std::string {
    const auto* device = resolveDevice(StringUtils::trim(args));
    if (device == nullptr) {
      return "error: no matching paired device\n";
    }
    if (!castSupportedFor(*device)) {
      return "error: device does not support casting\n";
    }
    const bool stopping = castAppleFor(*device) && m_cast.airplayRunning;
    launchCastMirror(device->id);
    return stopping ? "stopped\n" : "started\n";
  });
}

void PhoneConnectService::attachDaemon() {
  if (m_impl->daemon != nullptr && m_snapshot.available) {
    refresh();
    return;
  }
  try {
    m_impl->daemon = m_impl->makeProxy(std::string{kDaemonPath});
  } catch (const sdbus::Error& e) {
    kLog.warn("kdeconnect daemon proxy failed: {}", e.what());
    m_snapshot.available = false;
    notifyChanged();
    return;
  }
  m_snapshot.available = true;
  m_snapshot.activating = false;
  m_snapshot.error.clear();
  m_impl->watchDaemonSignals();
  m_impl->loadAnnouncedName();
  m_impl->requestDeviceList();
  if (m_impl->wantDiscovery) {
    m_impl->applyDiscovery(true);
  }
  notifyChanged();
}

void PhoneConnectService::detachDaemon() {
  m_impl->discoveryHeld = false;
  m_impl->daemon.reset();
  m_impl->deviceProxies.clear();
  m_impl->batteryProxies.clear();
  m_impl->shareProxies.clear();
  m_impl->sftpProxies.clear();
  m_impl->notificationProxies.clear();
  m_impl->inflight.clear();
  m_files = {};
  m_notifications.clear();
  m_snapshot = PhoneSnapshot{};
  notifyChanged();
}

void PhoneConnectService::notifyChanged() {
  if (m_destroying) {
    return;
  }
  m_changed.emit();
  // Device fields (paired, plugins) land asynchronously; re-check the always-on
  // receiver after every change, off the signal so arming never nests an emit.
  if (!m_armPending) {
    m_armPending = true;
    std::weak_ptr<void> lifetime = m_lifetime;
    DeferredCall::callLater([this, lifetime]() {
      if (lifetime.expired()) {
        return;
      }
      m_armPending = false;
      armReceiverIfWanted();
    });
  }
}

const PhoneDevice* PhoneConnectService::peerDevice(std::string_view preferredId) const {
  const PhoneDevice* preferred = nullptr;
  const PhoneDevice* reachable = nullptr;
  const PhoneDevice* paired = nullptr;
  for (const auto& device : m_snapshot.devices) {
    if (!device.paired) {
      continue;
    }
    if (paired == nullptr) {
      paired = &device;
    }
    if (device.reachable && reachable == nullptr) {
      reachable = &device;
    }
    if (!preferredId.empty() && device.id == preferredId) {
      preferred = &device;
    }
  }
  if (preferred != nullptr) {
    return preferred;
  }
  if (reachable != nullptr) {
    return reachable;
  }
  return paired;
}

void PhoneConnectService::refreshNotifications(const std::string& deviceId) {
  m_impl->refreshNotifications(deviceId);
}

void PhoneConnectService::dismissNotification(const std::string& deviceId, const std::string& publicId) {
  if (deviceId.empty() || publicId.empty()) {
    return;
  }
  m_impl->fireVoid(notificationObjectPath(deviceId, publicId), kNotificationItemInterface, "dismiss");
  std::erase_if(m_notifications, [&](const PhoneNotification& item) {
    return item.deviceId == deviceId && item.publicId == publicId;
  });
  notifyChanged();
}

PhoneDevice* PhoneConnectService::findDevice(const std::string& deviceId) {
  auto it = std::ranges::find(m_snapshot.devices, deviceId, &PhoneDevice::id);
  return it != m_snapshot.devices.end() ? &*it : nullptr;
}

const PhoneDevice* PhoneConnectService::findDevice(const std::string& deviceId) const {
  auto it = std::ranges::find(m_snapshot.devices, deviceId, &PhoneDevice::id);
  return it != m_snapshot.devices.end() ? &*it : nullptr;
}

const PhoneDevice* PhoneConnectService::resolveDevice(std::string_view query) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(query));
  const PhoneDevice* onlyPaired = nullptr;
  int pairedCount = 0;
  for (const auto& device : m_snapshot.devices) {
    if (!device.paired) {
      continue;
    }
    ++pairedCount;
    onlyPaired = &device;
    if (!needle.empty()
        && (device.id == query || StringUtils::toLower(device.name) == needle
            || StringUtils::toLower(device.name).find(needle) != std::string::npos)) {
      return &device;
    }
  }
  if (needle.empty() && pairedCount == 1) {
    return onlyPaired;
  }
  return nullptr;
}

bool PhoneConnectService::castAndroidFor(const PhoneDevice& device) const noexcept {
  return device.paired && device.sftpPlugin;
}

bool PhoneConnectService::castAppleFor(const PhoneDevice& device) const noexcept {
  // iOS KDE Connect has share/clipboard/battery but not SFTP storage browsing.
  return device.paired && !device.sftpPlugin
      && (device.type.empty() || device.type == "phone" || device.type == "tablet");
}

bool PhoneConnectService::castSupportedFor(const PhoneDevice& device) const noexcept {
  return castAndroidFor(device) || castAppleFor(device);
}

void PhoneConnectService::pollAppleMirror() {
  if (!m_airplayAlive) {
    m_airplayPollTimer.stop();
    if (m_cast.airplayRunning) {
      m_cast.airplayRunning = false;
      m_cast.mirrorStreaming = false;
      notifyChanged();
    }
    return;
  }
  pollPhonePointer();
}

// UxPlay log lines, already on the main thread. Session edges come from RAOP:
// mirroring starts when the phone's RTP mirror stream opens and ends on TEARDOWN or
// when its control socket closes (Stop Mirroring, lock screen timeout, walked away).
void PhoneConnectService::onAirplayLog(std::uint32_t generation, std::string line) {
  if (generation != m_airplayGeneration || !m_airplayAlive) {
    return;
  }
  bool streaming = m_cast.mirrorStreaming;
  if (line.find("raop_rtp_mirror starting mirroring") != std::string::npos) {
    streaming = true;
  } else if (line.find("TEARDOWN") != std::string::npos || line.find("Connection closed") != std::string::npos
             || line.find("raop_rtp_mirror stopped") != std::string::npos) {
    streaming = false;
  } else if (line.find("Client connected") != std::string::npos) {
    // A new client took the receiver over; frames follow shortly.
    m_pointerLastSeen = std::chrono::steady_clock::now();
  }
  if (streaming != m_cast.mirrorStreaming) {
    m_cast.mirrorStreaming = streaming;
    if (streaming) {
      m_pointerLastSeen = std::chrono::steady_clock::now();
      pollPhonePointer();
    }
    kLog.info("iPhone mirror {}", streaming ? "streaming" : "idle");
    notifyChanged();
  }
}

void PhoneConnectService::onAirplayExit(std::uint32_t generation, int exitCode) {
  if (generation != m_airplayGeneration) {
    return;
  }
  m_airplayAlive = false;
  m_airplayCancel.reset();
  m_airplayPollTimer.stop();
  const bool wasRunning = m_cast.airplayRunning;
  m_cast.airplayRunning = false;
  m_cast.mirrorStreaming = false;
  if (wasRunning && exitCode != 0) {
    kLog.warn("uxplay exited with {}", exitCode);
  }
  notifyChanged();
  // Crash or mDNS name clash while we still want it up: try again in a moment rather
  // than leaving Screen Mirroring empty. Deliberate stops clear the flag first.
  if (!m_receiverUserStopped) {
    m_receiverRearmTimer.start(std::chrono::seconds{3}, [this]() { armReceiverIfWanted(); });
  }
}

void PhoneConnectService::setPhoneNotifications(bool enabled) {
  if (m_phoneNotifications == enabled) {
    return;
  }
  m_phoneNotifications = enabled;
  if (m_cast.pointerRunning) {
    pollPhonePointer();
  }
}

void PhoneConnectService::setMirrorAlwaysOn(bool enabled) {
  if (m_mirrorAlwaysOn == enabled) {
    return;
  }
  m_mirrorAlwaysOn = enabled;
  if (enabled) {
    m_receiverUserStopped = false;
    armReceiverIfWanted();
  } else if (m_cast.receiverAuto && !m_cast.mirrorStreaming) {
    stopCastMirror();
  }
}

void PhoneConnectService::armReceiverIfWanted() {
  if (!m_mirrorAlwaysOn || m_receiverUserStopped || m_airplayAlive) {
    return;
  }
  const bool applePaired = std::ranges::any_of(m_snapshot.devices, [this](const PhoneDevice& device) {
    return castAppleFor(device);
  });
  if (!applePaired) {
    return;
  }
  if (!m_preflight.done) {
    runMirrorPreflight(); // arms from the preflight completion hop
    return;
  }
  if (!m_preflight.missing.empty()) {
    return;
  }
  kLog.info("iPhone paired: arming the AirPlay receiver");
  startAppleMirror(true);
}

void PhoneConnectService::restartReceiver() {
  const bool wanted = m_mirrorAlwaysOn && !m_receiverUserStopped;
  stopCastMirror();
  if (wanted) {
    // Give the old process a moment to release the AirPlay ports and mDNS name.
    m_receiverUserStopped = false;
    m_receiverRearmTimer.start(std::chrono::milliseconds{800}, [this]() { armReceiverIfWanted(); });
  }
}

// Status probe off the main thread: the socket round-trip plus the script's BlueZ query
// take tens of milliseconds, which at 1 Hz is a visible hitch in every animation.
void PhoneConnectService::pollPhonePointer() {
  if (m_pointerProbeInFlight) {
    return;
  }
  m_pointerProbeInFlight = true;
  std::weak_ptr<void> lifetime = m_lifetime;
  const bool wantNotifications = m_phoneNotifications;
  std::thread([this, lifetime, wantNotifications]() {
    auto status = phonePointerRequest({{"type", "status"}});
    if (!status.has_value()) {
      ensurePhonePointer();
    } else if (const auto ancs = status->find("ancs");
               ancs != status->end() && ancs->is_object() && ancs->value("enabled", true) != wantNotifications) {
      (void)phonePointerRequest({{"type", "ancs"}, {"enabled", wantNotifications}});
      (*ancs)["enabled"] = wantNotifications;
    }
    DeferredCall::callLater([this, lifetime, status = std::move(status)]() {
      if (lifetime.expired()) {
        return;
      }
      m_pointerProbeInFlight = false;
      if (applyPhonePointerStatus(status)) {
        notifyChanged();
      }
    });
  }).detach();
}

bool PhoneConnectService::applyPhonePointerStatus(const std::optional<nlohmann::json>& status) {
  const auto snapshot = std::make_tuple(
      m_cast.pointerRunning, m_cast.pointerConnected, m_cast.pointerKeyboard, m_cast.pointerConfigOk,
      m_cast.pointerBonded, m_cast.pointerBondLost, m_cast.ancsEnabled, m_cast.ancsAttached, m_cast.ancsActive,
      m_cast.ancsPosted
  );
  if (!status.has_value()) {
    m_cast.pointerRunning = false;
    m_cast.pointerConnected = false;
    m_cast.pointerKeyboard = false;
    m_cast.ancsAttached = false;
    m_cast.ancsActive = 0;
  } else {
    m_cast.pointerRunning = true;
    m_cast.pointerConnected = status->value("connected", false);
    m_cast.pointerKeyboard = status->value("keyboard_captured", false);
    m_cast.pointerConfigOk = status->value("bluetooth_config_ok", true);
    m_cast.pointerBonded = status->value("bonded", false);
    if (const auto ancs = status->find("ancs"); ancs != status->end() && ancs->is_object()) {
      m_cast.ancsEnabled = ancs->value("enabled", true);
      m_cast.ancsAttached = ancs->value("attached", false);
      m_cast.ancsActive = ancs->value("active", 0);
      m_cast.ancsPosted = ancs->value("posted", 0);
    } else {
      m_cast.ancsAttached = false;
      m_cast.ancsActive = 0;
    }
  }
  // iOS drops the bond silently (or AssistiveTouch is switched off). The phone is
  // demonstrably nearby while it mirrors, so a bonded pointer that stays disconnected
  // for a while during mirroring is the user-visible sign.
  const auto now = std::chrono::steady_clock::now();
  if (m_cast.pointerConnected || m_pointerLastSeen == std::chrono::steady_clock::time_point{}) {
    m_pointerLastSeen = now;
  }
  m_cast.pointerBondLost = m_cast.airplayRunning && m_cast.pointerRunning && m_cast.pointerBonded
      && !m_cast.pointerConnected && (now - m_pointerLastSeen) > std::chrono::seconds{20};
  return snapshot
      != std::make_tuple(
             m_cast.pointerRunning, m_cast.pointerConnected, m_cast.pointerKeyboard, m_cast.pointerConfigOk,
             m_cast.pointerBonded, m_cast.pointerBondLost, m_cast.ancsEnabled, m_cast.ancsAttached,
             m_cast.ancsActive, m_cast.ancsPosted
         );
}

void PhoneConnectService::phonePointerShortcut(const std::string& name) {
  const auto reply = phonePointerRequest({{"type", "shortcut"}, {"name", name}});
  if (!reply.has_value() || reply->contains("error")) {
    kLog.warn("phone pointer shortcut {} failed", name);
  }
}

void PhoneConnectService::setupPhonePointerBluetooth() {
  if (m_pointerSetupInFlight) {
    return;
  }
  const auto script = phoneScriptPath("helix-phone-bluetooth-setup.sh");
  if (!std::filesystem::exists(script) || !process::commandExists("pkexec")) {
    kLog.warn("bluetooth setup helper or pkexec is missing");
    return;
  }
  m_pointerSetupInFlight = true;
  std::weak_ptr<void> lifetime = m_lifetime;
  const bool started = process::runAsync(
      std::vector<std::string>{"pkexec", "/bin/sh", script.string()},
      process::RunCallbacks{
          .onExit =
              [this, lifetime](process::RunResult result) {
                // Still on the process worker: bluetoothd restarted, so the pointer script
                // must re-register its GATT service before the main thread re-probes it.
                const auto running = process::runSync({"pgrep", "-f", "helix-phone-hid.py"});
                if (running.exitCode == 0) {
                  (void)process::runSync({"pkill", "-f", "helix-phone-hid.py"});
                }
                ensurePhonePointer();
                DeferredCall::callLater([this, lifetime, result = std::move(result)]() {
                  if (lifetime.expired()) {
                    return;
                  }
                  m_pointerSetupInFlight = false;
                  if (result.exitCode != 0) {
                    kLog.warn("bluetooth setup for Helix Phone failed ({}): {}", result.exitCode, result.err);
                  }
                  pollPhonePointer();
                });
              },
      },
      process::RunOptions{.timeout = std::chrono::minutes{2}}
  );
  if (!started) {
    m_pointerSetupInFlight = false;
    kLog.warn("could not launch bluetooth setup for Helix Phone");
  }
}

// Probes everything the iPhone mirror depends on and picks the H.264 decoder. Runs on a
// worker; gst-inspect rebuilds the plugin registry on first use and can take seconds.
void PhoneConnectService::runMirrorPreflight() {
  if (m_preflight.done || m_preflight.inFlight) {
    return;
  }
  m_preflight.inFlight = true;
  std::weak_ptr<void> lifetime = m_lifetime;
  std::thread([this, lifetime]() {
    MirrorPreflight result;
    result.done = true;

    auto hasElement = [](const char* name) {
      const auto run = process::runSync(
          std::vector<std::string>{"gst-inspect-1.0", "--exists", name}, process::RunOptions{.timeout = std::chrono::seconds{20}}
      );
      return run.exitCode == 0;
    };

    if (!process::commandExists("uxplay")) {
      result.missing.push_back({
          .label = i18n::tr("control-center.phone.cast-need-uxplay"),
          .fix = "uxplay",
      });
    }
    if (!process::commandExists("gst-inspect-1.0")) {
      result.missing.push_back({
          .label = i18n::tr("control-center.phone.cast-need-gstreamer"),
          .fix = "gstreamer1 gstreamer1-plugins-base",
      });
    } else {
      // Hardware first, software last. The chosen element also decides whether the
      // pipeline needs a GPU download stage before videoconvert.
      for (const char* candidate : {"nvh264dec", "vah264dec", "vaapih264dec", "avdec_h264"}) {
        if (hasElement(candidate)) {
          result.decoder = candidate;
          break;
        }
      }
      if (result.decoder.empty()) {
        result.missing.push_back({
            .label = i18n::tr("control-center.phone.cast-need-decoder"),
            .fix = "gstreamer1-plugins-bad-free gstreamer1-plugin-libav",
        });
      }
      if (!hasElement("videoscale") || !hasElement("videoconvert") || !hasElement("filesink")) {
        result.missing.push_back({
            .label = i18n::tr("control-center.phone.cast-need-gst-base"),
            .fix = "gstreamer1-plugins-base",
        });
      }
    }
    if (!process::commandExists("python3")
        || process::runSync(std::vector<std::string>{"python3", "-c", "import dbus, gi; from gi.repository import GLib"})
                   .exitCode
               != 0) {
      result.missing.push_back({
          .label = i18n::tr("control-center.phone.cast-need-python"),
          .fix = "python3-dbus python3-gobject",
      });
    }
    std::error_code ec;
    if (!std::filesystem::exists("/sys/class/bluetooth/hci0", ec)) {
      result.missing.push_back({
          .label = i18n::tr("control-center.phone.cast-need-bluetooth"),
          .fix = i18n::tr("control-center.phone.cast-need-bluetooth-fix"),
      });
    } else if (process::runSync(std::vector<std::string>{"systemctl", "is-active", "--quiet", "bluetooth.service"}).exitCode != 0) {
      result.missing.push_back({
          .label = i18n::tr("control-center.phone.cast-need-bluetoothd"),
          .fix = "systemctl enable --now bluetooth",
      });
    }

    DeferredCall::callLater([this, lifetime, result = std::move(result)]() mutable {
      if (lifetime.expired()) {
        return;
      }
      m_preflight = std::move(result);
      std::string missing;
      for (const auto& item : m_preflight.missing) {
        missing += (missing.empty() ? "" : ", ") + item.fix;
      }
      kLog.info(
          "iPhone mirror preflight: decoder={} missing=[{}]", m_preflight.decoder.empty() ? "-" : m_preflight.decoder,
          missing
      );
      applyMirrorPreflight();
      notifyChanged();
      armReceiverIfWanted();
    });
  }).detach();
}

void PhoneConnectService::applyMirrorPreflight() {
  m_cast.preflightDone = m_preflight.done;
  m_cast.decoder = m_preflight.decoder;
  m_cast.missing = m_preflight.missing;
  m_cast.toolsReady = m_preflight.done ? m_preflight.missing.empty() : airplayToolsReady();
  if (m_preflight.done && !m_preflight.missing.empty()) {
    m_cast.error = i18n::tr("control-center.phone.cast-mirror-needs-setup");
  } else if (m_cast.error == i18n::tr("control-center.phone.cast-mirror-needs-setup")
             || m_cast.error == i18n::tr("control-center.phone.cast-airplay-missing")) {
    m_cast.error.clear();
  }
}

void PhoneConnectService::startAppleMirror(bool automatic) {
  if (!m_preflight.done) {
    runMirrorPreflight();
  }
  if (!airplayToolsReady() || (m_preflight.done && !m_preflight.missing.empty())) {
    applyMirrorPreflight();
    if (m_cast.error.empty()) {
      m_cast.error = i18n::tr("control-center.phone.cast-airplay-missing");
    }
    notifyChanged();
    return;
  }
  if (m_airplayAlive) {
    m_cast.airplayRunning = true;
    m_cast.toolsReady = true;
    notifyChanged();
    return;
  }
  ensurePhoneMirrorWindowRules();
  // UxPlay only decodes. Frames go out as fixed-size RGBA into a FIFO that the shell's
  // PhoneMirrorWindow reads and draws itself, so there is no UxPlay window (and no
  // Xwayland) involved; videoscale add-borders letterboxes rotation instead of squashing.
  // The FIFO must exist before filesink opens it; the mirror window creates it and
  // holds it O_RDWR so the sink never blocks or hits EOF between pipeline restarts.
  const auto fifo = phoneMirrorVideoPath();
  std::error_code fifoEc;
  if (!std::filesystem::is_fifo(fifo, fifoEc)) {
    std::filesystem::remove(fifo, fifoEc);
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
      kLog.warn("could not create {}", fifo.string());
    }
  }
  // Preflight picks the decoder; before it finishes (or when gst-inspect is missing)
  // fall back to nvh264dec, which is what this pipeline was tuned on.
  const std::string decoder = m_preflight.decoder.empty() ? std::string("nvh264dec") : m_preflight.decoder;
  const std::string download = decoder == "nvh264dec" ? "cudadownload ! " : "";
  std::vector<std::string> args;
  // UxPlay logs through stdio; when stdout is a pipe it block-buffers, and session
  // edges would arrive minutes late. stdbuf forces line buffering.
  if (process::commandExists("stdbuf")) {
    args = {"stdbuf", "-oL", "-eL"};
  }
  args.insert(args.end(), {
      "uxplay",
      "-n",
      std::string(compositors::niri::kPhoneMirrorWindowTitle),
      "-nh",
      "-fps",
      "60",
      "-vsync",
      "no",
      "-s",
      std::format("{}x{}", kPhoneMirrorFrameWidth, kPhoneMirrorFrameHeight),
      "-srgb",
      "no",
      "-vd",
      decoder,
      "-vc",
      std::format(
          "{}videoconvert ! videoscale add-borders=true ! "
          "video/x-raw,format=RGBA,width={},height={},pixel-aspect-ratio=1/1",
          download, kPhoneMirrorFrameWidth, kPhoneMirrorFrameHeight
      ),
      "-vs",
      std::format("filesink location={} sync=false async=false", fifo.string()),
  });

  const std::uint32_t generation = ++m_airplayGeneration;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  std::weak_ptr<void> lifetime = m_lifetime;
  // Worker-thread callbacks: split into lines there, hop to the main thread per line.
  auto lineReader = std::make_shared<std::string>();
  auto onChunk = [this, lifetime, generation, lineReader](std::string_view chunk) {
    lineReader->append(chunk);
    std::size_t start = 0;
    while (true) {
      const auto end = lineReader->find('\n', start);
      if (end == std::string::npos) {
        break;
      }
      std::string line = lineReader->substr(start, end - start);
      start = end + 1;
      DeferredCall::callLater([this, lifetime, generation, line = std::move(line)]() mutable {
        if (!lifetime.expired()) {
          onAirplayLog(generation, std::move(line));
        }
      });
    }
    lineReader->erase(0, start);
  };
  const bool launched = process::runAsync(
      args,
      process::RunCallbacks{
          .stdOut = onChunk,
          .stdErr = onChunk,
          .onExit =
              [this, lifetime, generation](process::RunResult result) {
                DeferredCall::callLater([this, lifetime, generation, exitCode = result.exitCode]() {
                  if (!lifetime.expired()) {
                    onAirplayExit(generation, exitCode);
                  }
                });
              },
      },
      process::RunOptions{.cancel = cancel}
  );
  if (!launched) {
    m_cast.error = i18n::tr("control-center.phone.cast-airplay-start-failed");
    m_cast.airplayRunning = false;
    notifyChanged();
    return;
  }
  m_airplayCancel = std::move(cancel);
  m_airplayAlive = true;
  m_cast.applePeer = true;
  m_cast.toolsReady = true;
  m_cast.airplayRunning = true;
  m_cast.receiverAuto = automatic;
  m_cast.mirrorStreaming = false;
  m_cast.airplayName = std::string(compositors::niri::kPhoneMirrorWindowTitle);
  m_cast.error.clear();
  m_pointerLastSeen = std::chrono::steady_clock::now();
  pollPhonePointer();
  m_airplayPollTimer.startRepeating(std::chrono::seconds{1}, [this]() { pollAppleMirror(); });
  notifyChanged();
}

void PhoneConnectService::stopCastMirror() {
  m_airplayPollTimer.stop();
  m_receiverRearmTimer.stop();
  if (m_airplayCancel != nullptr) {
    m_airplayCancel->store(true);
    m_airplayCancel.reset();
  }
  // The exit hop for this generation is now stale; bump so it is ignored.
  ++m_airplayGeneration;
  m_airplayAlive = false;
  if (m_cast.airplayRunning || m_cast.mirrorStreaming) {
    m_cast.airplayRunning = false;
    m_cast.mirrorStreaming = false;
    m_cast.receiverAuto = false;
    notifyChanged();
  }
}

void PhoneConnectService::loadCastState() {
  m_castAdbHosts.clear();
  m_castRecent.clear();
  std::ifstream in(castStatePath());
  if (!in) {
    return;
  }
  nlohmann::json json;
  try {
    in >> json;
  } catch (const nlohmann::json::exception&) {
    return;
  }
  if (!json.is_object()) {
    return;
  }
  if (const auto hosts = json.find("adb_hosts"); hosts != json.end() && hosts->is_object()) {
    for (const auto& [deviceId, host] : hosts->items()) {
      if (host.is_string() && !host.get<std::string>().empty()) {
        m_castAdbHosts[deviceId] = host.get<std::string>();
      }
    }
  }
  if (const auto recent = json.find("recent"); recent != json.end() && recent->is_object()) {
    for (const auto& [deviceId, apps] : recent->items()) {
      if (!apps.is_array()) {
        continue;
      }
      std::vector<PhoneCastApp> list;
      for (const auto& row : apps) {
        if (!row.is_object()) {
          continue;
        }
        PhoneCastApp app;
        app.package = row.value("package", std::string{});
        app.label = row.value("label", std::string{});
        if (app.package.empty()) {
          continue;
        }
        if (app.label.empty()) {
          app.label = labelFromPackage(app.package);
        }
        list.push_back(std::move(app));
        if (list.size() >= 8) {
          break;
        }
      }
      if (!list.empty()) {
        m_castRecent[deviceId] = std::move(list);
      }
    }
  }
}

void PhoneConnectService::saveCastState() {
  nlohmann::json json = nlohmann::json::object();
  json["adb_hosts"] = nlohmann::json::object();
  for (const auto& [id, savedHost] : m_castAdbHosts) {
    json["adb_hosts"][id] = savedHost;
  }
  json["recent"] = nlohmann::json::object();
  for (const auto& [id, apps] : m_castRecent) {
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& app : apps) {
      rows.push_back({{"package", app.package}, {"label", app.label}});
    }
    json["recent"][id] = std::move(rows);
  }
  const auto path = castStatePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) {
    return;
  }
  out << json.dump(2);
}

void PhoneConnectService::saveCastAdbHost(const std::string& deviceId, const std::string& host) {
  const std::string normalized = normalizeAdbHost(host);
  if (deviceId.empty() || normalized.empty()) {
    return;
  }
  m_castAdbHosts[deviceId] = normalized;
  saveCastState();
}

void PhoneConnectService::recordCastRecent(
    const std::string& deviceId, const std::string& package, const std::string& label
) {
  if (deviceId.empty() || package.empty()) {
    return;
  }
  auto& list = m_castRecent[deviceId];
  list.erase(
      std::ranges::remove_if(list, [&](const PhoneCastApp& app) { return app.package == package; }).begin(),
      list.end()
  );
  list.insert(list.begin(), PhoneCastApp{.package = package, .label = label.empty() ? labelFromPackage(package) : label});
  if (list.size() > 8) {
    list.resize(8);
  }
  saveCastState();
}

std::vector<std::string> PhoneConnectService::castHostCandidates(const std::string& deviceId) const {
  std::vector<std::string> hosts;
  const auto pushUnique = [&](std::string_view raw) {
    const std::string host = normalizeAdbHost(raw);
    if (host.empty()) {
      return;
    }
    if (std::ranges::find(hosts, host) != hosts.end()) {
      return;
    }
    hosts.push_back(host);
  };
  if (const auto it = m_castAdbHosts.find(deviceId); it != m_castAdbHosts.end()) {
    pushUnique(it->second);
  }
  if (const auto* device = findDevice(deviceId)) {
    for (const auto& address : device->reachableAddresses) {
      pushUnique(address);
    }
  }
  return hosts;
}

std::string PhoneConnectService::castSuggestedHost(const std::string& deviceId) const {
  const auto hosts = castHostCandidates(deviceId);
  return hosts.empty() ? std::string{} : hosts.front();
}

void PhoneConnectService::queueCastRefreshResult(PhoneCastSnapshot snapshot) {
  {
    std::lock_guard lock(m_castMutex);
    m_castPending = std::move(snapshot);
    m_castPendingReady = true;
  }
  // Timer callbacks run on the main loop; start from the worker is racy, so drain
  // also happens when the UI next asks for cast state via a short poll.
}

void PhoneConnectService::drainCastRefreshResult() {
  PhoneCastSnapshot pending;
  {
    std::lock_guard lock(m_castMutex);
    if (!m_castPendingReady) {
      return;
    }
    pending = std::move(m_castPending);
    m_castPendingReady = false;
  }
  m_castPollTimer.stop();
  m_castRefreshInFlight = false;
  if (pending.adbConnected && pending.adbSerial.contains(':')) {
    saveCastAdbHost(pending.deviceId, pending.adbSerial);
    pending.suggestedHost = pending.adbSerial;
  }
  m_cast = std::move(pending);
  if (m_cast.adbConnected && !m_cast.adbSerial.empty() && !m_cast.apps.empty()) {
    attachCachedCastIcons(m_cast.apps, m_cast.deviceId);
    scheduleCastIconFetch(m_cast.deviceId, m_cast.adbSerial, m_cast.apps);
  }
  notifyChanged();
}

void PhoneConnectService::refreshCastApps(const std::string& deviceId) {
  drainCastRefreshResult();
  if (m_castRefreshInFlight) {
    return;
  }
  const auto* device = findDevice(deviceId);
  if (device == nullptr || !castSupportedFor(*device)) {
    m_cast = PhoneCastSnapshot{.deviceId = deviceId, .suggestedHost = castSuggestedHost(deviceId)};
    notifyChanged();
    return;
  }

  if (castAppleFor(*device)) {
    const bool running = m_airplayAlive;
    const bool receiverAuto = m_cast.receiverAuto;
    const bool streaming = m_cast.mirrorStreaming;
    if (!running) {
      m_airplayPollTimer.stop();
    }
    m_cast = PhoneCastSnapshot{
        .deviceId = deviceId,
        .toolsReady = airplayToolsReady(),
        .applePeer = true,
        .airplayRunning = running,
        .receiverAuto = running && receiverAuto,
        .mirrorStreaming = running && streaming,
        .airplayName = std::string(compositors::niri::kPhoneMirrorWindowTitle),
    };
    runMirrorPreflight();
    applyMirrorPreflight();
    if (running) {
      (void)pollPhonePointer();
    }
    notifyChanged();
    return;
  }

  m_castRefreshInFlight = true;
  m_cast = PhoneCastSnapshot{
      .deviceId = deviceId,
      .loading = true,
      .toolsReady = castToolsReady(),
      .suggestedHost = castSuggestedHost(deviceId),
  };
  if (const auto recentIt = m_castRecent.find(deviceId); recentIt != m_castRecent.end()) {
    m_cast.recent = recentIt->second;
  }
  notifyChanged();

  const std::vector<std::string> hosts = castHostCandidates(deviceId);
  const std::vector<PhoneCastApp> recent =
      m_castRecent.contains(deviceId) ? m_castRecent[deviceId] : std::vector<PhoneCastApp>{};

  m_castPollTimer.startRepeating(std::chrono::milliseconds{50}, [this]() { drainCastRefreshResult(); });

  process::RunCallbacks callbacks;
  callbacks.onExit = [this, deviceId, hosts, recent](process::RunResult) {
    PhoneCastSnapshot next;
    next.deviceId = deviceId;
    next.toolsReady = castToolsReady();
    next.suggestedHost = hosts.empty() ? std::string{} : hosts.front();
    next.recent = recent;
    if (!next.toolsReady) {
      queueCastRefreshResult(std::move(next));
      return;
    }
    if (const auto serial = resolveAdbSerial(hosts)) {
      next.adbConnected = true;
      next.adbSerial = *serial;
      next.apps = applyCachedIcons(prioritizeRecentApps(listCastApps(next.adbSerial), recent), deviceId);
      if (serial->contains(':')) {
        // Persist on main thread via queued snapshot path.
        next.suggestedHost = *serial;
      }
    } else {
      next.error = i18n::tr("control-center.phone.cast-adb-missing");
    }
    queueCastRefreshResult(std::move(next));
  };
  (void)process::runAsync({"true"}, std::move(callbacks));
}

std::optional<std::string> PhoneConnectService::resolveCastSerial(const std::string& deviceId) {
  return resolveAdbSerial(castHostCandidates(deviceId));
}

void PhoneConnectService::connectCastAdb(const std::string& deviceId, std::string host, std::string pairCode) {
  drainCastRefreshResult();
  if (m_castRefreshInFlight) {
    return;
  }
  const auto* device = findDevice(deviceId);
  if (device == nullptr || !castAndroidFor(*device) || !castToolsReady()) {
    return;
  }
  std::string normalized = StringUtils::trim(host);
  if (normalized.empty()) {
    return;
  }
  if (normalized.find(':') == std::string::npos) {
    normalized = normalizeAdbHost(normalized);
  }
  const std::string code = StringUtils::trim(pairCode);
  if (code.empty()) {
    saveCastAdbHost(deviceId, normalized);
  }

  m_castRefreshInFlight = true;
  m_cast = PhoneCastSnapshot{
      .deviceId = deviceId,
      .loading = true,
      .toolsReady = true,
      .connecting = true,
      .suggestedHost = normalized,
  };
  if (const auto recentIt = m_castRecent.find(deviceId); recentIt != m_castRecent.end()) {
    m_cast.recent = recentIt->second;
  }
  notifyChanged();

  const std::vector<PhoneCastApp> recent = m_cast.recent;
  m_castPollTimer.startRepeating(std::chrono::milliseconds{50}, [this]() { drainCastRefreshResult(); });

  process::RunCallbacks callbacks;
  callbacks.onExit = [this, deviceId, normalized, code, recent](process::RunResult) {
    PhoneCastSnapshot next;
    next.deviceId = deviceId;
    next.toolsReady = true;
    next.suggestedHost = normalized;
    next.recent = recent;
    if (!code.empty()) {
      const process::RunResult paired =
          process::runSyncWithTimeout({"adb", "pair", normalized, code}, std::chrono::seconds{30});
      if (!paired) {
        next.error = i18n::tr("control-center.phone.cast-adb-pair-failed", "host", normalized);
        queueCastRefreshResult(std::move(next));
        return;
      }
    }
    if (const auto serial = resolveAdbSerial({normalized})) {
      next.adbConnected = true;
      next.adbSerial = *serial;
      next.apps = applyCachedIcons(prioritizeRecentApps(listCastApps(next.adbSerial), recent), deviceId);
      if (serial->contains(':')) {
        next.suggestedHost = *serial;
      }
    } else if (!code.empty()) {
      next.error = i18n::tr("control-center.phone.cast-adb-pair-need-connect");
    } else {
      next.error = i18n::tr("control-center.phone.cast-adb-connect-failed", "host", normalized);
    }
    queueCastRefreshResult(std::move(next));
  };
  (void)process::runAsync({"true"}, std::move(callbacks));
}

void PhoneConnectService::attachCachedCastIcons(std::vector<PhoneCastApp>& apps, const std::string& deviceId) const {
  apps = applyCachedIcons(std::move(apps), deviceId);
}

void PhoneConnectService::scheduleCastIconFetch(
    const std::string& deviceId, const std::string& serial, std::vector<PhoneCastApp> apps
) {
  if (m_castIconInFlight || serial.empty() || apps.empty() || !process::commandExists("unzip")) {
    return;
  }
  std::vector<std::string> missing;
  missing.reserve(std::min<std::size_t>(apps.size(), 12));
  for (const auto& app : apps) {
    if (!app.iconPath.empty()) {
      continue;
    }
    missing.push_back(app.package);
    if (missing.size() >= 12) {
      break;
    }
  }
  if (missing.empty()) {
    return;
  }

  m_castIconInFlight = true;
  m_castIconPollTimer.startRepeating(std::chrono::milliseconds{75}, [this]() { drainCastIconResults(); });

  process::RunCallbacks callbacks;
  callbacks.onExit = [this, deviceId, serial, missing = std::move(missing)](process::RunResult) {
    std::vector<std::pair<std::string, std::string>> ready;
    for (const auto& package : missing) {
      if (!fetchCastAppIcon(serial, deviceId, package)) {
        continue;
      }
      ready.emplace_back(package, castIconCachePath(deviceId, package).string());
    }
    {
      std::lock_guard lock(m_castMutex);
      m_castIconPending = std::move(ready);
      m_castIconPendingReady = true;
    }
  };
  (void)process::runAsync({"true"}, std::move(callbacks));
}

void PhoneConnectService::drainCastIconResults() {
  std::vector<std::pair<std::string, std::string>> ready;
  {
    std::lock_guard lock(m_castMutex);
    if (!m_castIconPendingReady) {
      return;
    }
    ready = std::move(m_castIconPending);
    m_castIconPendingReady = false;
  }
  m_castIconPollTimer.stop();
  m_castIconInFlight = false;
  if (ready.empty()) {
    return;
  }
  bool changed = false;
  for (const auto& [package, path] : ready) {
    for (auto& app : m_cast.apps) {
      if (app.package == package && app.iconPath != path) {
        app.iconPath = path;
        changed = true;
      }
    }
    for (auto& app : m_cast.recent) {
      if (app.package == package && app.iconPath != path) {
        app.iconPath = path;
        changed = true;
      }
    }
  }
  if (changed) {
    notifyChanged();
  }
}

void PhoneConnectService::launchCastMirror(const std::string& deviceId) {
  const auto* device = findDevice(deviceId);
  if (device == nullptr || !castSupportedFor(*device)) {
    return;
  }
  if (castAppleFor(*device)) {
    if (m_airplayAlive) {
      // Stop means stop: no auto re-arm until Start is pressed again.
      m_receiverUserStopped = true;
      stopCastMirror();
      return;
    }
    m_receiverUserStopped = false;
    startAppleMirror();
    return;
  }
  if (!castToolsReady()) {
    return;
  }
  const auto serial = resolveCastSerial(deviceId);
  if (!serial) {
    refreshCastApps(deviceId);
    return;
  }
  if (serial->contains(':')) {
    saveCastAdbHost(deviceId, *serial);
  }
  const std::string title = device->name.empty() ? device->id : device->name;
  auto args = scrcpyArgs(*serial, title, m_castAudio, {"--max-size=1080"});
  (void)process::runAsync(std::move(args));
}

void PhoneConnectService::launchCastApp(
    const std::string& deviceId, const std::string& package, const std::string& label
) {
  if (package.empty() || !castToolsReady()) {
    return;
  }
  const auto* device = findDevice(deviceId);
  if (device == nullptr || !castAndroidFor(*device)) {
    return;
  }
  const auto serial = resolveCastSerial(deviceId);
  if (!serial) {
    refreshCastApps(deviceId);
    return;
  }
  if (serial->contains(':')) {
    saveCastAdbHost(deviceId, *serial);
  }
  recordCastRecent(deviceId, package, label);
  const std::string windowTitle = label.empty() ? package : label;
  auto args = scrcpyArgs(*serial, windowTitle, m_castAudio, {"--new-display=1080x2400"});
  args.push_back("--start-app=" + package);
  (void)process::runAsync(std::move(args));
  if (m_cast.deviceId == deviceId) {
    m_cast.recent = m_castRecent[deviceId];
    m_cast.apps = prioritizeRecentApps(std::move(m_cast.apps), m_cast.recent);
    notifyChanged();
  }
}
