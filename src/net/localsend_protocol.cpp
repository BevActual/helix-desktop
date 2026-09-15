#include "net/localsend_protocol.h"

#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace localsend {
  namespace {

    [[nodiscard]] std::string jsonString(const nlohmann::json& obj, std::string_view key) {
      const auto it = obj.find(key);
      if (it == obj.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    [[nodiscard]] int jsonInt(const nlohmann::json& obj, std::string_view key, int fallback) {
      const auto it = obj.find(key);
      if (it == obj.end()) {
        return fallback;
      }
      if (it->is_number_integer()) {
        return static_cast<int>(it->get<std::int64_t>());
      }
      if (it->is_string()) {
        try {
          return std::stoi(it->get<std::string>());
        } catch (...) {
          return fallback;
        }
      }
      return fallback;
    }

    [[nodiscard]] bool jsonBool(const nlohmann::json& obj, std::string_view key, bool fallback) {
      const auto it = obj.find(key);
      if (it == obj.end()) {
        return fallback;
      }
      if (it->is_boolean()) {
        return it->get<bool>();
      }
      return fallback;
    }

    [[nodiscard]] DeviceInfo deviceFromJson(const nlohmann::json& obj) {
      DeviceInfo info;
      info.alias = jsonString(obj, "alias");
      info.version = jsonString(obj, "version");
      if (info.version.empty()) {
        info.version = std::string(kProtocolVersion);
      }
      info.deviceModel = jsonString(obj, "deviceModel");
      info.deviceType = jsonString(obj, "deviceType");
      if (info.deviceType.empty()) {
        info.deviceType = "desktop";
      }
      info.fingerprint = jsonString(obj, "fingerprint");
      info.port = jsonInt(obj, "port", kDefaultPort);
      if (info.port <= 0 || info.port > 65535) {
        info.port = kDefaultPort;
      }
      info.protocol = jsonString(obj, "protocol");
      if (info.protocol != "https") {
        info.protocol = "http";
      }
      info.download = jsonBool(obj, "download", false);
      info.announce = jsonBool(obj, "announce", false);
      return info;
    }

    [[nodiscard]] nlohmann::json deviceToJson(const DeviceInfo& info, bool includeAnnounce) {
      nlohmann::json obj = {
          {"alias", info.alias},
          {"version", info.version.empty() ? std::string(kProtocolVersion) : info.version},
          {"deviceType", info.deviceType.empty() ? "desktop" : info.deviceType},
          {"fingerprint", info.fingerprint},
          {"port", info.port > 0 ? info.port : kDefaultPort},
          {"protocol", info.protocol == "https" ? "https" : "http"},
          {"download", info.download},
      };
      if (!info.deviceModel.empty()) {
        obj["deviceModel"] = info.deviceModel;
      }
      if (includeAnnounce) {
        obj["announce"] = info.announce;
      }
      return obj;
    }

    [[nodiscard]] int hexValue(char ch) {
      if (ch >= '0' && ch <= '9') {
        return ch - '0';
      }
      if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
      }
      if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
      }
      return -1;
    }

    [[nodiscard]] std::string urlDecode(std::string_view in) {
      std::string out;
      out.reserve(in.size());
      for (std::size_t i = 0; i < in.size(); ++i) {
        const char ch = in[i];
        if (ch == '+') {
          out.push_back(' ');
          continue;
        }
        if (ch == '%' && i + 2 < in.size()) {
          const int hi = hexValue(in[i + 1]);
          const int lo = hexValue(in[i + 2]);
          if (hi >= 0 && lo >= 0) {
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
          }
        }
        out.push_back(ch);
      }
      return out;
    }

  } // namespace

  std::optional<DeviceInfo> parseDeviceJson(std::string_view jsonText) {
    try {
      const auto obj = nlohmann::json::parse(jsonText);
      if (!obj.is_object()) {
        return std::nullopt;
      }
      auto info = deviceFromJson(obj);
      if (info.alias.empty() && info.fingerprint.empty()) {
        return std::nullopt;
      }
      return info;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string serializeDeviceJson(const DeviceInfo& info, bool includeAnnounce) {
    return deviceToJson(info, includeAnnounce).dump();
  }

  std::optional<PrepareUploadRequest> parsePrepareUploadJson(std::string_view jsonText) {
    try {
      const auto obj = nlohmann::json::parse(jsonText);
      if (!obj.is_object()) {
        return std::nullopt;
      }
      PrepareUploadRequest request;
      if (const auto infoIt = obj.find("info"); infoIt != obj.end() && infoIt->is_object()) {
        request.info = deviceFromJson(*infoIt);
      }
      const auto filesIt = obj.find("files");
      if (filesIt == obj.end() || !filesIt->is_object()) {
        return std::nullopt;
      }
      for (auto it = filesIt->begin(); it != filesIt->end(); ++it) {
        if (!it->is_object()) {
          continue;
        }
        FileMeta file;
        file.id = jsonString(*it, "id");
        if (file.id.empty()) {
          file.id = it.key();
        }
        file.fileName = jsonString(*it, "fileName");
        file.fileType = jsonString(*it, "fileType");
        if (file.fileType.empty()) {
          file.fileType = "application/octet-stream";
        }
        file.sha256 = jsonString(*it, "sha256");
        if (const auto sizeIt = it->find("size"); sizeIt != it->end()) {
          if (sizeIt->is_number_unsigned()) {
            file.size = sizeIt->get<std::uint64_t>();
          } else if (sizeIt->is_number_integer()) {
            const auto value = sizeIt->get<std::int64_t>();
            file.size = value < 0 ? 0 : static_cast<std::uint64_t>(value);
          }
        }
        if (!file.id.empty() && !file.fileName.empty()) {
          request.files.push_back(std::move(file));
        }
      }
      if (request.files.empty()) {
        return std::nullopt;
      }
      return request;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string serializePrepareUploadRequestJson(const PrepareUploadRequest& request) {
    nlohmann::json files = nlohmann::json::object();
    for (const auto& file : request.files) {
      nlohmann::json entry = {
          {"id", file.id},
          {"fileName", file.fileName},
          {"size", file.size},
          {"fileType", file.fileType.empty() ? "application/octet-stream" : file.fileType},
      };
      if (!file.sha256.empty()) {
        entry["sha256"] = file.sha256;
      }
      files[file.id] = std::move(entry);
    }
    nlohmann::json obj = {
        {"info", deviceToJson(request.info, false)},
        {"files", std::move(files)},
    };
    return obj.dump();
  }

  std::optional<PrepareUploadResponse> parsePrepareUploadResponseJson(std::string_view jsonText) {
    try {
      const auto obj = nlohmann::json::parse(jsonText);
      if (!obj.is_object()) {
        return std::nullopt;
      }
      PrepareUploadResponse response;
      response.sessionId = jsonString(obj, "sessionId");
      const auto filesIt = obj.find("files");
      if (filesIt != obj.end() && filesIt->is_object()) {
        for (auto it = filesIt->begin(); it != filesIt->end(); ++it) {
          if (it->is_string()) {
            response.fileTokens[it.key()] = it->get<std::string>();
          }
        }
      }
      if (response.sessionId.empty() || response.fileTokens.empty()) {
        return std::nullopt;
      }
      return response;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string serializePrepareUploadResponseJson(const PrepareUploadResponse& response) {
    nlohmann::json files = nlohmann::json::object();
    for (const auto& [id, token] : response.fileTokens) {
      files[id] = token;
    }
    return nlohmann::json{{"sessionId", response.sessionId}, {"files", std::move(files)}}.dump();
  }

  QueryParams parseQuery(std::string_view query) {
    QueryParams params;
    std::size_t start = 0;
    while (start < query.size()) {
      const auto amp = query.find('&', start);
      const auto part = query.substr(start, amp == std::string_view::npos ? query.size() - start : amp - start);
      const auto eq = part.find('=');
      const auto key = urlDecode(eq == std::string_view::npos ? part : part.substr(0, eq));
      const auto value = urlDecode(eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1));
      if (key == "pin") {
        params.pin = value;
      } else if (key == "sessionId") {
        params.sessionId = value;
      } else if (key == "fileId") {
        params.fileId = value;
      } else if (key == "token") {
        params.token = value;
      }
      if (amp == std::string_view::npos) {
        break;
      }
      start = amp + 1;
    }
    return params;
  }

  std::string mimeFromFileName(std::string_view fileName) {
    const auto dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= fileName.size()) {
      return "application/octet-stream";
    }
    std::string ext = StringUtils::toLower(fileName.substr(dot + 1));
    if (ext == "png") {
      return "image/png";
    }
    if (ext == "jpg" || ext == "jpeg") {
      return "image/jpeg";
    }
    if (ext == "gif") {
      return "image/gif";
    }
    if (ext == "webp") {
      return "image/webp";
    }
    if (ext == "svg") {
      return "image/svg+xml";
    }
    if (ext == "pdf") {
      return "application/pdf";
    }
    if (ext == "txt" || ext == "md") {
      return "text/plain";
    }
    if (ext == "json") {
      return "application/json";
    }
    if (ext == "html" || ext == "htm") {
      return "text/html";
    }
    if (ext == "mp4") {
      return "video/mp4";
    }
    if (ext == "webm") {
      return "video/webm";
    }
    if (ext == "mp3") {
      return "audio/mpeg";
    }
    if (ext == "wav") {
      return "audio/wav";
    }
    if (ext == "zip") {
      return "application/zip";
    }
    return "application/octet-stream";
  }

  std::string sanitizeFileName(std::string_view fileName) {
    std::string name(fileName);
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char ch) {
                 return ch == 0 || ch == '/' || ch == '\\' || ch == ':';
               }),
               name.end());
    if (name.empty() || name == "." || name == "..") {
      return "file";
    }
    if (name.size() > 180) {
      name.resize(180);
    }
    return name;
  }

  std::string peerKey(std::string_view ip, int port, std::string_view fingerprint) {
    if (!fingerprint.empty()) {
      return std::string(fingerprint);
    }
    return std::string(ip) + ":" + std::to_string(port);
  }

  bool isLocalSendApiPath(std::string_view path, std::string_view route) {
    const auto matches = [&](std::string_view prefix) {
      if (!path.starts_with(prefix)) {
        return false;
      }
      return path.substr(prefix.size()) == route;
    };
    return matches(kApiPrefixV2) || matches(kApiPrefixV1);
  }

  namespace {

    [[nodiscard]] bool isHiddenName(const std::filesystem::path& path) {
      const auto name = path.filename().string();
      return name.empty() || name.starts_with('.');
    }

    void collectFromDir(
        CollectSendFilesResult& result, const std::filesystem::path& dir, int depth, std::size_t maxFiles, int maxDepth
    ) {
      if (result.truncated) {
        return;
      }
      std::error_code itEc;
      for (const auto& entry : std::filesystem::directory_iterator(
               dir, std::filesystem::directory_options::skip_permission_denied, itEc
           )) {
        if (itEc) {
          return;
        }
        if (result.files.size() >= maxFiles) {
          result.truncated = true;
          return;
        }
        const auto path = entry.path();
        if (isHiddenName(path)) {
          continue;
        }
        std::error_code typeEc;
        if (entry.is_regular_file(typeEc) && !typeEc) {
          result.files.push_back(path);
          continue;
        }
        if (depth < maxDepth && entry.is_directory(typeEc) && !typeEc) {
          collectFromDir(result, path, depth + 1, maxFiles, maxDepth);
        }
      }
    }

  } // namespace

  CollectSendFilesResult collectSendFiles(const std::filesystem::path& root, std::size_t maxFiles, int maxDepth) {
    CollectSendFilesResult result;
    if (maxFiles == 0 || maxDepth < 1) {
      return result;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(root, ec) && !ec) {
      result.files.push_back(root);
      return result;
    }
    if (!std::filesystem::is_directory(root, ec) || ec) {
      return result;
    }
    collectFromDir(result, root, 1, maxFiles, maxDepth);
    std::ranges::sort(result.files);
    if (result.files.size() > maxFiles) {
      result.files.resize(maxFiles);
      result.truncated = true;
    }
    return result;
  }

} // namespace localsend
