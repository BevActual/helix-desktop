#pragma once

#include "render/core/thumbnail_service.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Button;
class Flex;
class Glyph;
class Image;
class Label;
class Renderer;
class ThumbnailService;

namespace control_center {

  enum class RecentCaptureKind : std::uint8_t { Images, Videos };

  class RecentCaptureGallery {
  public:
    struct Host {
      Flex* list = nullptr;
      Label* empty = nullptr;
      ThumbnailService* thumbnails = nullptr;
      float scale = 1.0F;
      RecentCaptureKind kind = RecentCaptureKind::Images;
      bool showAnnotate = false;
      std::function<void(const std::string& path)> onOpen;
      std::function<void(const std::string& path)> onAnnotate;
      std::function<void(const std::string& path)> onRead;
      std::function<void(const std::string& path)> onDescribe;
      std::function<void(std::string_view message)> onStatus;
    };

    void bind(Host host);
    void rebuild(const std::filesystem::path& directory);
    void update(Renderer& renderer);
    void onClose();

  private:
    struct Row {
      std::string path;
      Image* image = nullptr;
      Glyph* glyph = nullptr;
      Button* deleteButton = nullptr;
      bool thumbnailEligible = false;
    };

    void releaseThumbnails();
    void applyDeleteChrome();
    void requestDelete(const std::string& path);
    void refreshThumbnails(Renderer& renderer);
    [[nodiscard]] std::vector<std::string> extensions() const;

    Host m_host;
    std::filesystem::path m_directory;
    std::vector<Row> m_rows;
    std::string m_lastKey;
    std::string m_pendingDeletePath;
    ThumbnailService::Subscription m_pendingUploadSub;
    bool m_thumbnailRefreshPending = false;
    float m_thumbWidth = 0.0F;
    float m_thumbHeight = 0.0F;
  };

} // namespace control_center
