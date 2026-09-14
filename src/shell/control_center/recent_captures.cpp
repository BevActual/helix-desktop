#include "shell/control_center/recent_captures.h"

#include "core/files/directory_scanner.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <filesystem>
#include <system_error>
#include <utility>

namespace control_center {
  namespace {

    constexpr std::size_t kRecentFileLimit = 5;
    constexpr float kThumbWidth = 96.0F;
    constexpr float kThumbHeight = 54.0F;

  } // namespace

  void RecentCaptureGallery::bind(Host host) {
    onClose();
    m_host = std::move(host);
    m_thumbWidth = kThumbWidth * m_host.scale;
    m_thumbHeight = kThumbHeight * m_host.scale;
    if (m_host.thumbnails != nullptr) {
      m_pendingUploadSub = m_host.thumbnails->subscribePendingUpload([this]() {
        if (m_host.list == nullptr) {
          return;
        }
        m_thumbnailRefreshPending = true;
        PanelManager::instance().requestUpdateOnly();
      });
    }
  }

  void RecentCaptureGallery::onClose() {
    releaseThumbnails();
    m_pendingUploadSub.disconnect();
    m_host = {};
    m_rows.clear();
    m_lastKey.clear();
    m_pendingDeletePath.clear();
    m_thumbnailRefreshPending = false;
  }

  std::vector<std::string> RecentCaptureGallery::extensions() const {
    if (m_host.kind == RecentCaptureKind::Videos) {
      return {".mp4"};
    }
    return {".png"};
  }

  void RecentCaptureGallery::releaseThumbnails() {
    if (m_host.thumbnails == nullptr) {
      return;
    }
    for (const auto& row : m_rows) {
      if (row.thumbnailEligible) {
        m_host.thumbnails->release(row.path);
      }
    }
  }

  void RecentCaptureGallery::applyDeleteChrome() {
    for (auto& row : m_rows) {
      if (row.deleteButton == nullptr) {
        continue;
      }
      const bool pending = row.path == m_pendingDeletePath;
      row.deleteButton->setGlyph(pending ? "warning" : "trash");
      row.deleteButton->setVariant(pending ? ButtonVariant::Destructive : ButtonVariant::Ghost);
      row.deleteButton->setTooltip(
          i18n::tr(pending ? "control-center.captures.delete-again" : "control-center.captures.delete")
      );
    }
  }

  void RecentCaptureGallery::requestDelete(const std::string& path) {
    if (path.empty()) {
      return;
    }
    if (m_pendingDeletePath != path) {
      m_pendingDeletePath = path;
      applyDeleteChrome();
      PanelManager::instance().requestRedraw();
      return;
    }

    std::error_code ec;
    if (!std::filesystem::remove(path, ec) || ec) {
      if (m_host.onStatus) {
        m_host.onStatus(i18n::tr("control-center.captures.delete-failed"));
      }
      return;
    }
    m_pendingDeletePath.clear();
    rebuild(m_directory);
  }

  void RecentCaptureGallery::rebuild(const std::filesystem::path& directory) {
    if (m_host.list == nullptr || m_host.empty == nullptr) {
      return;
    }
    m_directory = directory;

    DirectoryScanner scanner;
    const auto entries = scanner.scan(
        directory, extensions(), false, FileDialogSortField::Modified, FileDialogSortOrder::Descending
    );

    std::string key;
    std::size_t shown = 0;
    for (const auto& entry : entries) {
      if (entry.isDir) {
        continue;
      }
      key += entry.absPath.string();
      key.push_back('\n');
      if (++shown >= kRecentFileLimit) {
        break;
      }
    }
    if (key == m_lastKey) {
      return;
    }

    uiAssertNotRendering("RecentCaptureGallery::rebuild");
    releaseThumbnails();
    m_rows.clear();
    while (!m_host.list->children().empty()) {
      m_host.list->removeChild(m_host.list->children().front().get());
    }

    const float scale = m_host.scale;
    const bool images = m_host.kind == RecentCaptureKind::Images;
    shown = 0;
    for (const auto& entry : entries) {
      if (entry.isDir) {
        continue;
      }

      Row row;
      row.path = entry.absPath.string();
      row.thumbnailEligible = images && m_host.thumbnails != nullptr;
      if (row.thumbnailEligible) {
        (void)m_host.thumbnails->acquire(row.path);
      }

      const std::string path = row.path;
      auto thumb = ui::column({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
          .radius = Style::scaledRadiusMd(scale),
          .clipChildren = true,
          .width = m_thumbWidth,
          .height = m_thumbHeight,
      });
      thumb->addChild(
          ui::image({
              .out = &row.image,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusMd(scale),
              .width = m_thumbWidth,
              .height = m_thumbHeight,
              .visible = false,
              .participatesInLayout = false,
              .configure =
                  [](Image& image) {
                    image.setHitTestVisible(false);
                  },
          })
      );
      thumb->addChild(
          ui::glyph({
              .out = &row.glyph,
              .glyph = images ? "photo" : "movie",
              .glyphSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );

      auto meta = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .flexGrow = 1.0F,
      });
      meta->addChild(
          ui::button({
              .text = entry.name,
              .contentAlign = ButtonContentAlign::Start,
              .variant = ButtonVariant::Ghost,
              .tooltip = formatFileTime(entry.mtime),
              .minHeight = Style::controlHeightSm * scale,
              .paddingH = Style::spaceSm * scale,
              .flexGrow = 1.0F,
              .onClick =
                  [this, path]() {
                    m_pendingDeletePath.clear();
                    applyDeleteChrome();
                    if (m_host.onOpen) {
                      m_host.onOpen(path);
                    }
                  },
          })
      );
      meta->addChild(
          ui::label({
              .text = formatFileTime(entry.mtime),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
          })
      );

      auto actions = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceXs * scale,
      });
      if (m_host.showAnnotate && m_host.onAnnotate) {
        actions->addChild(
            ui::button({
                .glyph = "brush",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("control-center.screenshot.annotate"),
                .minWidth = Style::controlHeightSm * scale,
                .minHeight = Style::controlHeightSm * scale,
                .padding = Style::spaceXs * scale,
                .onClick =
                    [this, path]() {
                      m_pendingDeletePath.clear();
                      applyDeleteChrome();
                      m_host.onAnnotate(path);
                    },
            })
        );
      }
      actions->addChild(
          ui::button({
              .out = &row.deleteButton,
              .glyph = "trash",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.captures.delete"),
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .onClick = [this, path]() { requestDelete(path); },
          })
      );

      m_host.list->addChild(ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .fillWidth = true,
          },
          std::move(thumb), std::move(meta), std::move(actions)
      ));
      m_rows.push_back(std::move(row));

      if (++shown >= kRecentFileLimit) {
        break;
      }
    }

    if (!m_pendingDeletePath.empty()) {
      bool stillPresent = false;
      for (const auto& row : m_rows) {
        if (row.path == m_pendingDeletePath) {
          stillPresent = true;
          break;
        }
      }
      if (!stillPresent) {
        m_pendingDeletePath.clear();
      }
    }
    applyDeleteChrome();

    m_host.empty->setVisible(shown == 0);
    m_host.list->setVisible(shown > 0);
    m_lastKey = std::move(key);
    m_thumbnailRefreshPending = shown > 0;
    PanelManager::instance().requestLayout();
  }

  void RecentCaptureGallery::refreshThumbnails(Renderer& renderer) {
    if (m_host.thumbnails == nullptr) {
      return;
    }
    for (auto& row : m_rows) {
      if (!row.thumbnailEligible || row.image == nullptr) {
        continue;
      }
      const TextureHandle handle = m_host.thumbnails->peek(row.path);
      if (handle.id == 0) {
        row.image->clear(renderer);
        row.image->setVisible(false);
        if (row.glyph != nullptr) {
          row.glyph->setVisible(true);
        }
        continue;
      }
      row.image->setExternalTexture(renderer, handle);
      row.image->setVisible(true);
      row.image->setPosition(0.0F, 0.0F);
      row.image->setSize(m_thumbWidth, m_thumbHeight);
      if (row.glyph != nullptr) {
        row.glyph->setVisible(false);
      }
    }
  }

  void RecentCaptureGallery::update(Renderer& renderer) {
    if (m_host.thumbnails == nullptr || m_host.list == nullptr) {
      return;
    }
    if (m_thumbnailRefreshPending || !m_rows.empty()) {
      (void)m_host.thumbnails->uploadPending(renderer.textureManager());
      m_thumbnailRefreshPending = false;
      refreshThumbnails(renderer);
    }
  }

} // namespace control_center
