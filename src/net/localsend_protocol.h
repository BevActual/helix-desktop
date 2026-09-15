#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace localsend {

  inline constexpr std::string_view kProtocolVersion = "2.1";
  inline constexpr std::string_view kMulticastAddress = "224.0.0.167";
  inline constexpr int kDefaultPort = 53317;
  inline constexpr std::string_view kApiPrefixV2 = "/api/localsend/v2";
  inline constexpr std::string_view kApiPrefixV1 = "/api/localsend/v1";

  struct DeviceInfo {
    std::string alias;
    std::string version = std::string(kProtocolVersion);
    std::string deviceModel;
    std::string deviceType = "desktop";
    std::string fingerprint;
    int port = kDefaultPort;
    std::string protocol = "http";
    bool download = false;
    bool announce = false;
  };

  struct FileMeta {
    std::string id;
    std::string fileName;
    std::uint64_t size = 0;
    std::string fileType = "application/octet-stream";
    std::string sha256;
  };

  struct PrepareUploadRequest {
    DeviceInfo info;
    std::vector<FileMeta> files;
  };

  struct PrepareUploadResponse {
    std::string sessionId;
    std::unordered_map<std::string, std::string> fileTokens;
  };

  struct QueryParams {
    std::string pin;
    std::string sessionId;
    std::string fileId;
    std::string token;
  };

  [[nodiscard]] std::optional<DeviceInfo> parseDeviceJson(std::string_view jsonText);
  [[nodiscard]] std::string serializeDeviceJson(const DeviceInfo& info, bool includeAnnounce);
  [[nodiscard]] std::optional<PrepareUploadRequest> parsePrepareUploadJson(std::string_view jsonText);
  [[nodiscard]] std::string serializePrepareUploadRequestJson(const PrepareUploadRequest& request);
  [[nodiscard]] std::optional<PrepareUploadResponse> parsePrepareUploadResponseJson(std::string_view jsonText);
  [[nodiscard]] std::string serializePrepareUploadResponseJson(const PrepareUploadResponse& response);

  [[nodiscard]] QueryParams parseQuery(std::string_view query);
  [[nodiscard]] std::string mimeFromFileName(std::string_view fileName);
  [[nodiscard]] std::string sanitizeFileName(std::string_view fileName);
  [[nodiscard]] std::string peerKey(std::string_view ip, int port, std::string_view fingerprint);
  [[nodiscard]] bool isLocalSendApiPath(std::string_view path, std::string_view route);

} // namespace localsend
