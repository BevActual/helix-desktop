#include "shell/control_center/tab_search.h"

#include "util/string_utils.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace control_center {
  namespace {

    struct AliasRow {
      std::string_view id;
      std::string_view aliases;
    };

    constexpr AliasRow kAliases[] = {
        {"theme", "appearance color palette colours colors look custom customize accent"},
        {"type", "font typeface typography"},
        {"scale", "size ui-scale zoom interface-scale"},
        {"roundness", "corners rounded radius border-radius"},
        {"effects", "contrast motion animation borders shadows tint icons glass transparency"},
        {"language", "locale translation i18n lang"},
        {"windows", "gaps struts columns tiling niri windows"},
        {"niri", "setup compositor autostart"},
        {"audio", "sound volume mic microphone speaker"},
        {"bluetooth", "bt nearby paired"},
        {"calendar", "clock clocks timezone timezones world-clock date accounts caldav google ical ics vdir"},
        {"clipboard", "paste clips cliphist"},
        {"control-center", "cc"},
        {"keybinds", "binds shortcuts keyboard hotkeys attention"},
        {"desktop", "widgets clock hot-corners corners"},
        {"dock", "taskbar pinned"},
        {"launcher", "runner app-launcher type-to-launch overview"},
        {"media", "mpris player music now-playing artwork album shuffle repeat visualizer spectrum"},
        {"monitor", "display displays brightness night-light nightlight gamma"},
        {"network", "wifi wi-fi wireless internet ethernet vpn cellular"},
        {"notifications", "notify notification dnd do-not-disturb history clear filter today yesterday"},
        {"packages", "package packages updates rpm dnf pkcon store flatpak firmware fwupd"},
        {"plugins", "plugin extension extensions addons"},
        {"power", "battery batteries sleep suspend lock caffeine idle profile health charging peripherals shutdown reboot logout"},
        {"capture", "screenshot screen-shot shot record recorder recording screen-record"},
        {"screen-time", "screentime usage"},
        {"ai", "ai-usage ai llm tokens quota claude cursor copilot codex gemini ollama"},
        {"system", "resources cpu memory ram disk"},
        {"tailscale", "vpn tailnet"},
        {"localsend", "nearby share airdrop send files"},
        {"phone", "kdeconnect kde-connect gsconnect android ios pair ping ring"},
        {"syncthing", "sync folders devices backup"},
        {"firewall", "firewalld zone panic ports services ssh"},
        {"printers", "printer cups print jobs queue"},
        {"cast", "cast sharing rdp miracast chromecast wireless display tv"},
        {"camera", "webcam webcam-preview exposure white-balance focus fov uvc v4l2"},
        {"privacy", "microphone camera webcam screen share capture mic location telemetry ping"},
        {"templates", "template rice rices"},
        {"wallpaper", "background wallpapers overview backdrop"},
        {"weather", "forecast temperature location"},
    };

    std::string fold(std::string_view text) { return StringUtils::toLower(text); }

    std::string idVariants(std::string_view id) {
      std::string out = fold(id);
      for (char& ch : out) {
        if (ch == '-' || ch == '_' || ch == ':' || ch == '/') {
          ch = ' ';
        }
      }
      return out;
    }

    int fieldScore(std::string_view haystack, std::string_view query) {
      if (haystack.empty() || query.empty()) {
        return 0;
      }
      if (haystack == query) {
        return 100;
      }
      if (haystack.starts_with(query)) {
        return 80;
      }
      if (haystack.find(query) != std::string_view::npos) {
        return 50;
      }
      return 0;
    }

    int aliasScore(std::string_view id, std::string_view query) {
      for (const auto& row : kAliases) {
        if (row.id != id) {
          continue;
        }
        int best = 0;
        std::size_t start = 0;
        const auto aliases = fold(row.aliases);
        while (start <= aliases.size()) {
          const auto end = aliases.find(' ', start);
          const auto token = std::string_view(aliases).substr(
              start, end == std::string_view::npos ? aliases.size() - start : end - start
          );
          best = std::max(best, fieldScore(token, query));
          if (end == std::string_view::npos) {
            break;
          }
          start = end + 1;
        }
        return best;
      }
      return 0;
    }

  } // namespace

  int tabSearchScore(std::string_view id, std::string_view title, std::string_view query) {
    const std::string needle = fold(StringUtils::trim(query));
    if (needle.size() < 2) {
      return 0;
    }
    const std::string titleFold = fold(title);
    const std::string idFold = fold(id);
    const std::string idSpaced = idVariants(id);
    int best = fieldScore(titleFold, needle);
    best = std::max(best, fieldScore(idFold, needle));
    best = std::max(best, fieldScore(idSpaced, needle));
    best = std::max(best, aliasScore(id, needle));
    return best;
  }

} // namespace control_center
