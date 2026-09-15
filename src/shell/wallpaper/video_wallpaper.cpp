#include "shell/wallpaper/video_wallpaper.h"

#include "core/process/process.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace wallpaper::video {
  namespace {

    [[nodiscard]] nlohmann::json assignmentObject(const nlohmann::json& decoded) {
      if (!decoded.is_object()) {
        return nlohmann::json::object();
      }
      if (decoded.contains("assignments") && decoded["assignments"].is_object()) {
        return decoded["assignments"];
      }
      return decoded;
    }

  } // namespace

  bool binaryAvailable() { return process::commandExists("mpvpaper"); }

  bool helpSupportsAutoMode(std::string_view helpText) { return helpText.find("--auto-mode") != std::string_view::npos; }

  std::filesystem::path resolveDirectory(std::string_view configured) {
    const std::string trimmed = StringUtils::trim(configured);
    if (!trimmed.empty()) {
      return FileUtils::expandUserPath(trimmed);
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / "Videos";
    }
    return {};
  }

  std::string mpvOptionString(const LaunchSpec& spec) {
    std::string options = "loop-file=inf panscan=1.0";
    if (spec.mute) {
      options += " no-audio";
    }
    options += spec.hardwareDecode ? " hwdec=auto" : " hwdec=no";
    const std::string extra = StringUtils::trim(spec.extraMpvOptions);
    if (!extra.empty()) {
      options += ' ';
      options += extra;
    }
    return options;
  }

  std::vector<std::string> autoPauseArgs(const LaunchSpec& spec) {
    if (spec.autoPause == WallpaperVideoAutoPause::Off) {
      return {};
    }
    if (!spec.autoModeSupported) {
      return {"--auto-pause"};
    }
    if (spec.autoPause == WallpaperVideoAutoPause::Max) {
      return {"--auto-pause", "--auto-mode", "MAX"};
    }
    return {"--auto-pause", "--auto-mode", "FULL"};
  }

  std::vector<std::string> buildCommand(const LaunchSpec& spec) {
    std::vector<std::string> args{std::string(kBinary)};
    const auto pause = autoPauseArgs(spec);
    args.insert(args.end(), pause.begin(), pause.end());
    args.emplace_back("-o");
    args.push_back(mpvOptionString(spec));
    args.push_back(spec.connector.empty() ? std::string(kWildcardConnector) : spec.connector);
    args.push_back(spec.path);
    return args;
  }

  std::unordered_map<std::string, std::string> parseAssignmentsJson(std::string_view json) {
    std::unordered_map<std::string, std::string> out;
    if (StringUtils::trim(json).empty()) {
      return out;
    }
    try {
      const auto decoded = nlohmann::json::parse(json, nullptr, false);
      if (decoded.is_discarded()) {
        return out;
      }
      const auto assignments = assignmentObject(decoded);
      for (auto it = assignments.begin(); it != assignments.end(); ++it) {
        if (!it.value().is_string()) {
          continue;
        }
        const std::string connector = it.key();
        const std::string path = it.value().get<std::string>();
        if (connector.empty() || path.empty()) {
          continue;
        }
        out.insert_or_assign(connector, path);
      }
    } catch (const nlohmann::json::exception&) {
    }
    return out;
  }

  std::string serializeAssignmentsJson(const std::unordered_map<std::string, std::string>& assignments) {
    nlohmann::json encoded = {{"assignments", nlohmann::json::object()}};
    for (const auto& [connector, path] : assignments) {
      if (!connector.empty() && !path.empty()) {
        encoded["assignments"][connector] = path;
      }
    }
    return encoded.dump();
  }

} // namespace wallpaper::video
