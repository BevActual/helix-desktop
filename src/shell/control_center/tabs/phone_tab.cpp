#include "shell/control_center/tabs/phone_tab.h"

#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/kdeconnect/phone_connect_service.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr std::size_t kFileLimit = 48;
  constexpr float kFileCell = 88.0F;
  constexpr std::size_t kGridOverscan = 2;

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] const char* glyphForType(std::string_view type) {
    if (type == "tablet") {
      return "device-tablet";
    }
    if (type == "laptop") {
      return "device-laptop";
    }
    if (type == "desktop" || type == "computer") {
      return "device-desktop";
    }
    if (type == "tv") {
      return "device-tv";
    }
    return "device-mobile";
  }

  [[nodiscard]] std::string deviceDetail(const PhoneDevice& device) {
    if (device.pairRequestedByPeer) {
      if (!device.verificationKey.empty()) {
        return i18n::tr("control-center.phone.pair-incoming-code", "code", device.verificationKey);
      }
      return i18n::tr("control-center.phone.pair-incoming");
    }
    if (device.pairRequested) {
      if (!device.verificationKey.empty()) {
        return i18n::tr("control-center.phone.pair-waiting-code", "code", device.verificationKey);
      }
      return i18n::tr("control-center.phone.pair-waiting");
    }
    if (!device.paired) {
      return device.reachable ? i18n::tr("control-center.phone.available") : i18n::tr("control-center.phone.offline");
    }
    std::string out =
        device.reachable ? i18n::tr("control-center.phone.online") : i18n::tr("control-center.phone.offline");
    if (device.hasBattery && device.charge >= 0) {
      out += "  ·  ";
      out += std::to_string(device.charge);
      out += "%";
      if (device.charging) {
        out += "  ·  ";
        out += i18n::tr("control-center.phone.charging");
      }
    }
    return out;
  }

  [[nodiscard]] bool directoryPreferred(std::string_view label) {
    const std::string lower = StringUtils::toLower(std::string(label));
    return lower.contains("camera") || lower.contains("dcim") || lower.contains("picture")
        || lower.contains("photo") || lower.contains("download");
  }

  [[nodiscard]] std::string localPathFromUrl(std::string_view url) {
    constexpr std::string_view kPrefix = "file://";
    if (url.size() >= kPrefix.size() && url.substr(0, kPrefix.size()) == kPrefix) {
      return std::string(url.substr(kPrefix.size()));
    }
    return std::string(url);
  }

  void collectImages(
      DirectoryScanner& scanner, const std::vector<std::string>& extensions, const std::filesystem::path& dir,
      int depth, std::vector<std::string>& out
  ) {
    if (out.size() >= kFileLimit || depth > 2 || dir.empty()) {
      return;
    }
    const auto entries =
        scanner.scan(dir, extensions, false, FileDialogSortField::Modified, FileDialogSortOrder::Descending);
    std::vector<std::filesystem::path> subdirs;
    for (const auto& entry : entries) {
      if (entry.isDir) {
        subdirs.push_back(entry.absPath);
        continue;
      }
      out.push_back(entry.absPath.string());
      if (out.size() >= kFileLimit) {
        return;
      }
    }
    for (const auto& subdir : subdirs) {
      collectImages(scanner, extensions, subdir, depth + 1, out);
      if (out.size() >= kFileLimit) {
        return;
      }
    }
  }

  void applySelectionFill(Flex& tile, bool selected, bool hovered) {
    if (selected) {
      tile.setFill(colorSpecFromRole(ColorRole::Primary));
    } else if (hovered) {
      tile.setFill(colorSpecFromRole(ColorRole::Hover));
    } else {
      tile.clearFill();
    }
  }

  class PhoneFileTile final : public Flex {
  public:
    explicit PhoneFileTile(float scale) {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Center);
      setJustify(FlexJustify::Center);
      setRadius(Style::scaledRadiusMd(scale));
      setClipChildren(true);
      addChild(ui::image({
          .out = &m_image,
          .fit = ImageFit::Cover,
          .radius = Style::scaledRadiusMd(scale),
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Image& image) { image.setHitTestVisible(false); },
      }));
      addChild(ui::glyph({
          .out = &m_glyph,
          .glyph = "photo",
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .configure = [](Glyph& glyph) { glyph.setHitTestVisible(false); },
      }));
    }

    void bind(Renderer* renderer, const std::string& path, ThumbnailService* thumbnails, bool selected, bool hovered) {
      applySelectionFill(*this, selected, hovered);
      const float size = std::min(width(), height());
      if (m_image != nullptr) {
        m_image->setSize(size, size);
      }
      if (thumbnails == nullptr || renderer == nullptr || path.empty()) {
        if (m_image != nullptr) {
          m_image->setVisible(false);
        }
        if (m_glyph != nullptr) {
          m_glyph->setVisible(true);
        }
        return;
      }
      const TextureHandle handle = thumbnails->peek(path);
      if (handle.id == 0) {
        if (m_image != nullptr) {
          m_image->clear(*renderer);
          m_image->setVisible(false);
        }
        if (m_glyph != nullptr) {
          m_glyph->setVisible(true);
        }
        return;
      }
      if (m_image != nullptr) {
        m_image->setExternalTexture(*renderer, handle);
        m_image->setVisible(true);
      }
      if (m_glyph != nullptr) {
        m_glyph->setVisible(false);
      }
    }

  private:
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
  };

  class PhoneFilesAdapter final : public VirtualGridAdapter {
  public:
    PhoneFilesAdapter(float scale, ThumbnailService* thumbnails) : m_scale(scale), m_thumbnails(thumbnails) {}

    void setPaths(const std::vector<std::string>* paths) { m_paths = paths; }
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }

    [[nodiscard]] std::size_t itemCount() const override { return m_paths == nullptr ? 0 : m_paths->size(); }

    [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<PhoneFileTile>(m_scale); }

    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
      if (m_paths == nullptr || index >= m_paths->size()) {
        return;
      }
      static_cast<PhoneFileTile&>(tile).bind(m_renderer, (*m_paths)[index], m_thumbnails, selected, hovered);
    }

    void onActivate(std::size_t index) override {
      if (m_onActivate) {
        m_onActivate(index);
      }
    }

    [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
      if (m_paths == nullptr || index >= m_paths->size()) {
        return {};
      }
      return std::filesystem::path((*m_paths)[index]).filename().string();
    }

  private:
    float m_scale = 1.0F;
    ThumbnailService* m_thumbnails = nullptr;
    Renderer* m_renderer = nullptr;
    const std::vector<std::string>* m_paths = nullptr;
    std::function<void(std::size_t)> m_onActivate;
  };

  class CastAppIcon final : public Flex {
  public:
    explicit CastAppIcon(float scale) {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Center);
      setJustify(FlexJustify::Center);
      setSize(Style::controlHeightSm * scale, Style::controlHeightSm * scale);
      setHitTestVisible(false);
      addChild(ui::image({
          .out = &m_image,
          .fit = ImageFit::Cover,
          .radius = Style::scaledRadiusSm(scale),
          .width = Style::controlHeightSm * scale,
          .height = Style::controlHeightSm * scale,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Image& image) { image.setHitTestVisible(false); },
      }));
      addChild(ui::glyph({
          .out = &m_glyph,
          .glyph = "apps",
          .glyphSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .configure = [](Glyph& glyph) { glyph.setHitTestVisible(false); },
      }));
    }

    void bind(Renderer* renderer, const std::string& path, ThumbnailService* thumbnails, bool recent) {
      if (m_glyph != nullptr) {
        m_glyph->setGlyph(recent ? "history" : "apps");
      }
      if (thumbnails == nullptr || renderer == nullptr || path.empty()) {
        if (m_image != nullptr) {
          m_image->setVisible(false);
        }
        if (m_glyph != nullptr) {
          m_glyph->setVisible(true);
        }
        return;
      }
      const TextureHandle handle = thumbnails->peek(path, 64);
      if (handle.id == 0) {
        if (m_image != nullptr) {
          m_image->clear(*renderer);
          m_image->setVisible(false);
        }
        if (m_glyph != nullptr) {
          m_glyph->setVisible(true);
        }
        return;
      }
      if (m_image != nullptr) {
        m_image->setExternalTexture(*renderer, handle);
        m_image->setVisible(true);
      }
      if (m_glyph != nullptr) {
        m_glyph->setVisible(false);
      }
    }

  private:
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
  };

} // namespace

PhoneTab::Catalog PhoneTab::catalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Catalog::Notifications;
  case 2:
    return Catalog::Continue;
  case 3:
    return Catalog::Cast;
  default:
    return Catalog::Files;
  }
}

std::size_t PhoneTab::indexFromCatalog(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Notifications:
    return 1;
  case Catalog::Continue:
    return 2;
  case Catalog::Cast:
    return 3;
  case Catalog::Files:
    break;
  }
  return 0;
}

PhoneTab::PhoneTab(const ControlCenterServices& services)
    : m_service(services.phone), m_clipboard(services.clipboard), m_config(services.config),
      m_thumbnails(services.thumbnails) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("phone_tab", "catalog")) {
      if (*catalog == "continue") {
        m_catalog = Catalog::Continue;
      } else if (*catalog == "notifications") {
        m_catalog = Catalog::Notifications;
      } else if (*catalog == "cast") {
        m_catalog = Catalog::Cast;
      }
    }
    if (const std::optional<std::string> peer = m_config->stateString("phone_tab", "peer")) {
      m_peerId = *peer;
    }
  }
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

PhoneTab::~PhoneTab() = default;

std::unique_ptr<Flex> PhoneTab::create() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto well = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity](Flex& section) { applyCompactCardStyle(section, scale, opacity, false); },
  });
  well->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  well->addChild(
      ui::label({
          .out = &m_detail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  well->addChild(
      ui::button({
          .out = &m_activateButton,
          .text = i18n::tr("control-center.phone.start"),
          .glyph = "device-mobile",
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->tryActivate();
                }
              },
      })
  );
  well->addChild(
      ui::column({
          .out = &m_deviceList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );

  std::vector<ui::SegmentedOption> sourceOptions{
      {.label = i18n::tr("control-center.phone.source-files"), .glyph = "photo"},
      {.label = i18n::tr("control-center.phone.source-notifications"), .glyph = "bell"},
      {.label = i18n::tr("control-center.phone.source-continue"), .glyph = "share"},
      {.label = i18n::tr("control-center.phone.source-cast"), .glyph = "cast"},
  };
  auto chips = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
  });
  chips->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options = std::move(sourceOptions),
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = opacity,
          .equalSegmentWidths = false,
          .onChange = [this](std::size_t index) { applyCatalog(index); },
          .configure =
              [](Segmented& sources) {
                sources.setWrap(true);
                sources.setFillWidth(false);
              },
      })
  );
  well->addChild(std::move(chips));
  live->addChild(std::move(well));

  auto leftover = ui::column({
      .align = FlexAlign::Stretch,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });

  m_filesAdapter = std::make_unique<PhoneFilesAdapter>(scale, m_thumbnails);
  static_cast<PhoneFilesAdapter*>(m_filesAdapter.get())->setPaths(&m_filePaths);
  static_cast<PhoneFilesAdapter*>(m_filesAdapter.get())->setOnActivate([this](std::size_t index) {
    if (index < m_filePaths.size()) {
      pullRemoteFile(m_filePaths[index]);
    }
  });
  if (m_thumbnails != nullptr) {
    m_thumbnailSub = m_thumbnails->subscribePendingUpload([this]() {
      m_thumbnailRefreshPending = true;
      PanelManager::instance().requestUpdateOnly();
    });
  }

  auto filesHost = ui::column({
      .out = &m_filesHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  filesHost->addChild(
      ui::label({
          .out = &m_filesEmpty,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
      })
  );
  filesHost->addChild(ui::virtualGridView({
      .out = &m_filesGrid,
      .contentScale = scale,
      .columns = 0,
      .minCellWidth = kFileCell * scale,
      .squareCells = true,
      .columnGap = Style::spaceXs * scale,
      .rowGap = Style::spaceXs * scale,
      .overscanRows = kGridOverscan,
      .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
      .scrollbarVisible = true,
      .adapter = m_filesAdapter.get(),
      .flexGrow = 1.0F,
  }));
  leftover->addChild(std::move(filesHost));

  auto notificationsHost = ui::column({
      .out = &m_notificationsHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
  });
  notificationsHost->addChild(
      ui::label({
          .out = &m_notificationsEmpty,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
      })
  );
  auto notificationsScroll = ui::scrollView({
      .out = &m_notificationsScroll,
      .state = &m_notificationsScrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  notificationsScroll->content()->setDirection(FlexDirection::Vertical);
  notificationsScroll->content()->setAlign(FlexAlign::Stretch);
  notificationsScroll->content()->setGap(Style::spaceXs * scale);
  m_notificationsList = notificationsScroll->content();
  notificationsHost->addChild(std::move(notificationsScroll));
  leftover->addChild(std::move(notificationsHost));

  auto continueHost = ui::column({
      .out = &m_continueHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
  });
  continueHost->addChild(
      ui::label({
          .out = &m_continueEmpty,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
      })
  );
  auto continueScroll = ui::scrollView({
      .out = &m_leftoverScroll,
      .state = &m_leftoverScrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  continueScroll->content()->setDirection(FlexDirection::Vertical);
  continueScroll->content()->setAlign(FlexAlign::Stretch);
  continueScroll->content()->setGap(Style::spaceXs * scale);
  m_continueList = continueScroll->content();
  continueHost->addChild(std::move(continueScroll));
  leftover->addChild(std::move(continueHost));

  auto castHost = ui::column({
      .out = &m_castHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
  });
  castHost->addChild(
      ui::label({
          .out = &m_castEmpty,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 9,
      })
  );
  auto castAdbRow = ui::row({
      .out = &m_castAdbRow,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
  });
  castAdbRow->addChild(
      ui::input({
          .out = &m_castHostInput,
          .placeholder = i18n::tr("control-center.phone.cast-host-placeholder"),
          .surfaceOpacity = 1.0F,
          .flexGrow = 1.0F,
          .onSubmit =
              [this](const std::string&) {
                if (m_service == nullptr || m_castHostInput == nullptr) {
                  return;
                }
                const std::string peerId = peerDeviceId();
                if (!peerId.empty()) {
                  const std::string code = m_castPairInput != nullptr ? m_castPairInput->value() : std::string{};
                  m_service->connectCastAdb(peerId, m_castHostInput->value(), code);
                }
              },
      })
  );
  castAdbRow->addChild(
      ui::input({
          .out = &m_castPairInput,
          .placeholder = i18n::tr("control-center.phone.cast-pair-placeholder"),
          .minLayoutWidth = Style::controlHeight * scale * 3.0F,
          .surfaceOpacity = 1.0F,
          .onSubmit =
              [this](const std::string&) {
                if (m_service == nullptr || m_castHostInput == nullptr) {
                  return;
                }
                const std::string peerId = peerDeviceId();
                if (!peerId.empty()) {
                  const std::string code = m_castPairInput != nullptr ? m_castPairInput->value() : std::string{};
                  m_service->connectCastAdb(peerId, m_castHostInput->value(), code);
                }
              },
      })
  );
  castAdbRow->addChild(
      ui::button({
          .out = &m_castConnectButton,
          .text = i18n::tr("control-center.phone.cast-connect"),
          .glyph = "plug-connected",
          .variant = ButtonVariant::Secondary,
          .minHeight = Style::controlHeight * scale,
          .onClick =
              [this]() {
                if (m_service == nullptr || m_castHostInput == nullptr) {
                  return;
                }
                const std::string peerId = peerDeviceId();
                if (!peerId.empty()) {
                  const std::string code = m_castPairInput != nullptr ? m_castPairInput->value() : std::string{};
                  m_service->connectCastAdb(peerId, m_castHostInput->value(), code);
                }
              },
      })
  );
  castHost->addChild(std::move(castAdbRow));
  castHost->addChild(
      ui::input({
          .out = &m_castSearchInput,
          .placeholder = i18n::tr("control-center.phone.cast-search-placeholder"),
          .surfaceOpacity = 1.0F,
          .visible = false,
          .participatesInLayout = false,
          .onChange =
              [this](const std::string& value) {
                m_castQuery = value;
                m_castSignature.clear();
                rebuildCast();
                PanelManager::instance().requestLayout();
              },
      })
  );
  castHost->addChild(
      ui::button({
          .out = &m_castMirrorButton,
          .text = i18n::tr("control-center.phone.cast-mirror"),
          .glyph = "cast",
          .variant = ButtonVariant::Secondary,
          .minHeight = Style::controlHeight * scale,
          .visible = false,
          .participatesInLayout = false,
          .onClick =
              [this]() {
                if (m_service == nullptr) {
                  return;
                }
                const std::string peerId = peerDeviceId();
                if (peerId.empty()) {
                  return;
                }
                const auto& cast = m_service->castSnapshot();
                if (cast.applePeer && cast.mirrorStreaming) {
                  m_service->restartReceiver();
                  return;
                }
                const bool stopping = cast.applePeer && cast.airplayRunning;
                m_service->launchCastMirror(peerId);
                notify::info(
                    "Helix", i18n::tr("control-center.phone.title"),
                    stopping ? i18n::tr("control-center.phone.cast-airplay-stopped")
                             : (cast.applePeer ? i18n::tr("control-center.phone.cast-airplay-started")
                                               : i18n::tr("control-center.phone.cast-mirroring"))
                );
              },
      })
  );
  // [phone] mirror_always_on: leftover grouped row, iPhone peers only.
  auto alwaysOnRow = ui::row({
      .out = &m_castAlwaysOnRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
  });
  alwaysOnRow->addChild(ui::label({
      .text = i18n::tr("control-center.phone.cast-always-on"),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .flexGrow = 1.0F,
  }));
  alwaysOnRow->addChild(ui::toggle({
      .out = &m_castAlwaysOn,
      .checkedImmediate = false,
      .toggleSize = ToggleSize::Medium,
      .scale = scale,
  }));
  m_castAlwaysOn->setOnChange([this](bool value) {
    if (m_config != nullptr) {
      (void)m_config->setOverride({"phone", "mirror_always_on"}, value);
    }
    if (m_service != nullptr) {
      m_service->setMirrorAlwaysOn(value);
    }
  });
  castHost->addChild(std::move(alwaysOnRow));
  // iPhone pointer: a module well of functional plates (status line + phone shortcuts),
  // shown only while AirPlay is running. Not a second glass layer.
  auto pointerWell = ui::column({
      .out = &m_castPointerWell,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& section) { applyModuleWellStyle(section, scale); },
  });
  pointerWell->addChild(
      ui::label({
          .out = &m_castPointerStatus,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  auto pointerActions = ui::row({
      .out = &m_castPointerActions,
      .align = FlexAlign::Stretch,
      .wrap = true,
      .gap = Style::spaceSm * scale,
  });
  const float plateHeight = Style::controlHeight * scale;
  const float platePad = Style::spaceMd * scale;
  auto pointerPlate = [this, plateHeight, platePad](const char* key, const char* glyph, const char* shortcut) {
    return ui::button({
        .text = i18n::tr(key),
        .glyph = glyph,
        .variant = ButtonVariant::Ghost,
        .minHeight = plateHeight,
        .paddingH = platePad,
        .flexGrow = 1.0F,
        .onClick =
            [this, shortcut]() {
              if (m_service != nullptr) {
                m_service->phonePointerShortcut(shortcut);
              }
            },
    });
  };
  pointerActions->addChild(pointerPlate("control-center.phone.cast-pointer-home", "home", "home"));
  pointerActions->addChild(pointerPlate("control-center.phone.cast-pointer-search", "search", "search"));
  pointerActions->addChild(pointerPlate("control-center.phone.cast-pointer-library", "apps", "library"));
  pointerActions->addChild(
      pointerPlate("control-center.phone.cast-pointer-control-center", "adjustments", "control_center")
  );
  pointerWell->addChild(std::move(pointerActions));
  pointerWell->addChild(
      ui::button({
          .out = &m_castPointerSetup,
          .text = i18n::tr("control-center.phone.cast-pointer-setup"),
          .glyph = "bluetooth",
          .variant = ButtonVariant::Secondary,
          .tooltip = i18n::tr("control-center.phone.cast-pointer-setup-hint"),
          .minHeight = plateHeight,
          .visible = false,
          .participatesInLayout = false,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->setupPhonePointerBluetooth();
                }
              },
      })
  );
  castHost->addChild(std::move(pointerWell));
  auto castScroll = ui::scrollView({
      .out = &m_castScroll,
      .state = &m_castScrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  castScroll->content()->setDirection(FlexDirection::Vertical);
  castScroll->content()->setAlign(FlexAlign::Stretch);
  castScroll->content()->setGap(Style::spaceXs * scale);
  m_castList = castScroll->content();
  castHost->addChild(std::move(castScroll));
  leftover->addChild(std::move(castHost));
  live->addChild(std::move(leftover));

  syncCatalog();
  syncUi();
  return live;
}

std::unique_ptr<Flex> PhoneTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.phone.refresh"),
          .onClick =
              [this]() {
                if (m_service == nullptr) {
                  return;
                }
                if (!m_service->available()) {
                  m_service->tryActivate();
                }
                m_service->refresh();
                const std::string peerId = peerDeviceId();
                if (m_catalog == Catalog::Files) {
                  refreshFiles();
                } else if (m_catalog == Catalog::Notifications && !peerId.empty()) {
                  m_service->refreshNotifications(peerId);
                } else if (m_catalog == Catalog::Cast && !peerId.empty()) {
                  m_service->refreshCastApps(peerId);
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void PhoneTab::setActive(bool active) {
  if (m_service == nullptr) {
    return;
  }
  m_service->setDiscovery(active);
  if (active) {
    if (!m_service->available()) {
      m_service->tryActivate();
    }
    m_service->refresh();
    const std::string peerId = peerDeviceId();
    if (m_catalog == Catalog::Files) {
      refreshFiles();
    } else if (m_catalog == Catalog::Notifications && !peerId.empty()) {
      m_service->refreshNotifications(peerId);
    } else if (m_catalog == Catalog::Cast && !peerId.empty()) {
      m_service->refreshCastApps(peerId);
    }
  }
}

void PhoneTab::onClose() {
  if (m_service != nullptr) {
    m_service->setDiscovery(false);
  }
  if (m_thumbnails != nullptr) {
    for (const auto& path : m_filePaths) {
      m_thumbnails->release(path);
    }
  }
  m_thumbnailSub.disconnect();
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_activateButton = nullptr;
  m_deviceList = nullptr;
  m_catalogSwitch = nullptr;
  m_filesHost = nullptr;
  m_notificationsHost = nullptr;
  m_continueHost = nullptr;
  m_castHost = nullptr;
  m_filesEmpty = nullptr;
  m_notificationsEmpty = nullptr;
  m_continueEmpty = nullptr;
  m_castEmpty = nullptr;
  m_castMirrorButton = nullptr;
  m_castAlwaysOnRow = nullptr;
  m_castAlwaysOn = nullptr;
  m_castPointerWell = nullptr;
  m_castPointerStatus = nullptr;
  m_castPointerActions = nullptr;
  m_castPointerSetup = nullptr;
  m_castAdbRow = nullptr;
  m_castHostInput = nullptr;
  m_castPairInput = nullptr;
  m_castSearchInput = nullptr;
  m_castConnectButton = nullptr;
  m_notificationsList = nullptr;
  m_continueList = nullptr;
  m_castList = nullptr;
  m_filesGrid = nullptr;
  m_leftoverScroll = nullptr;
  m_notificationsScroll = nullptr;
  m_castScroll = nullptr;
  m_filesAdapter.reset();
  m_filePaths.clear();
  m_deviceSignature.clear();
  m_continueSignature.clear();
  m_notificationsSignature.clear();
  m_castSignature.clear();
  m_castHostPrefill.clear();
  m_castQuery.clear();
  releaseCastIcons();
  m_filesSignature.clear();
}

void PhoneTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void PhoneTab::doUpdate(Renderer& renderer) {
  if (auto* adapter = static_cast<PhoneFilesAdapter*>(m_filesAdapter.get())) {
    adapter->setRenderer(&renderer);
  }
  if (m_thumbnails != nullptr) {
    (void)m_thumbnails->uploadPending(renderer.textureManager());
  }
  for (const auto& bind : m_castIconBinds) {
    if (bind.icon == nullptr) {
      continue;
    }
    static_cast<CastAppIcon*>(bind.icon)->bind(&renderer, bind.path, m_thumbnails, bind.recent);
  }
  if (m_thumbnailRefreshPending && m_filesGrid != nullptr) {
    m_thumbnailRefreshPending = false;
    m_filesGrid->notifyDataChanged();
  }
}

void PhoneTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void PhoneTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  const char* value = "files";
  if (m_catalog == Catalog::Continue) {
    value = "continue";
  } else if (m_catalog == Catalog::Notifications) {
    value = "notifications";
  } else if (m_catalog == Catalog::Cast) {
    value = "cast";
  }
  (void)m_config->setStateString("phone_tab", "catalog", value);
}

void PhoneTab::persistPeer() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("phone_tab", "peer", m_peerId);
}

void PhoneTab::applyCatalog(std::size_t index) {
  if (m_syncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  if (next == m_catalog) {
    return;
  }
  m_catalog = next;
  persistCatalog();
  syncCatalog();
  if (m_catalog == Catalog::Files) {
    refreshFiles();
  } else if (m_catalog == Catalog::Notifications) {
    const std::string peerId = peerDeviceId();
    if (m_service != nullptr && !peerId.empty()) {
      m_service->refreshNotifications(peerId);
    }
    rebuildNotifications();
  } else if (m_catalog == Catalog::Cast) {
    const std::string peerId = peerDeviceId();
    if (m_service != nullptr && !peerId.empty()) {
      m_service->refreshCastApps(peerId);
    }
    rebuildCast();
  } else {
    rebuildContinue();
  }
  PanelManager::instance().requestLayout();
}

void PhoneTab::syncCatalog() {
  m_syncing = true;
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_catalog)) {
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_catalog));
  }
  m_syncing = false;
  (void)setShown(m_filesHost, m_catalog == Catalog::Files);
  (void)setShown(m_notificationsHost, m_catalog == Catalog::Notifications);
  (void)setShown(m_continueHost, m_catalog == Catalog::Continue);
  (void)setShown(m_castHost, m_catalog == Catalog::Cast);
}

std::string PhoneTab::peerDeviceId() const {
  if (m_service == nullptr) {
    return m_peerId;
  }
  if (const auto* peer = m_service->peerDevice(m_peerId)) {
    return peer->id;
  }
  return {};
}

void PhoneTab::selectPeer(std::string deviceId) {
  if (deviceId == m_peerId) {
    return;
  }
  m_peerId = std::move(deviceId);
  persistPeer();
  m_filesDeviceId.clear();
  m_filesSignature.clear();
  m_notificationsSignature.clear();
  if (m_service != nullptr && !m_peerId.empty()) {
    m_service->refreshNotifications(m_peerId);
  }
  rebuildDevices();
  if (m_catalog == Catalog::Files) {
    refreshFiles();
  } else if (m_catalog == Catalog::Notifications) {
    rebuildNotifications();
  } else if (m_catalog == Catalog::Cast) {
    if (m_service != nullptr && !m_peerId.empty()) {
      m_service->refreshCastApps(m_peerId);
    }
    rebuildCast();
  }
  PanelManager::instance().requestLayout();
}

void PhoneTab::refreshFiles() {
  const std::string deviceId = peerDeviceId();
  if (m_service == nullptr || deviceId.empty()) {
    m_filesDeviceId.clear();
    if (m_thumbnails != nullptr) {
      for (const auto& path : m_filePaths) {
        m_thumbnails->release(path);
      }
    }
    m_filePaths.clear();
    m_filesSignature.clear();
    if (m_filesGrid != nullptr) {
      m_filesGrid->notifyDataChanged();
    }
    if (m_filesEmpty != nullptr) {
      m_filesEmpty->setText(i18n::tr("control-center.phone.files-empty"));
      (void)setShown(m_filesEmpty, true);
    }
    return;
  }

  const auto& files = m_service->filesSnapshot();
  const auto* peer = m_service->peerDevice(m_peerId);
  const bool peerReachable = peer != nullptr && peer->reachable;
  if (m_filesDeviceId != deviceId) {
    m_filesDeviceId = deviceId;
    m_service->mountFiles(deviceId);
  } else if (!files.mounted && !files.mounting && peerReachable) {
    // Retry only when the peer is online. Offline mount errors notifyChanged; remounting
    // on every syncUi would recurse forever.
    m_service->mountFiles(deviceId);
  }

  if (m_filesEmpty != nullptr) {
    if (files.mounting) {
      m_filesEmpty->setText(i18n::tr("control-center.phone.files-mounting"));
    } else if (!files.error.empty()) {
      m_filesEmpty->setText(files.error);
    } else {
      m_filesEmpty->setText(i18n::tr("control-center.phone.files-empty"));
    }
  }

  std::string signature = files.deviceId;
  signature += files.mounted ? '1' : '0';
  signature += files.mounting ? '1' : '0';
  signature += files.error;
  for (const auto& [label, path] : files.directories) {
    signature += label;
    signature += path;
  }
  if (signature == m_filesSignature) {
    (void)setShown(m_filesEmpty, m_filePaths.empty());
    return;
  }
  m_filesSignature = std::move(signature);

  if (m_thumbnails != nullptr) {
    for (const auto& path : m_filePaths) {
      m_thumbnails->release(path);
    }
  }
  m_filePaths.clear();
  if (files.mounted && !files.directories.empty()) {
    DirectoryScanner scanner;
    const auto extensions = DirectoryScanner::imageExtensionFilter(false);
    std::vector<std::pair<std::string, std::string>> dirs = files.directories;
    std::stable_partition(dirs.begin(), dirs.end(), [](const auto& dir) { return directoryPreferred(dir.first); });
    for (const auto& [label, path] : dirs) {
      collectImages(scanner, extensions, path, 0, m_filePaths);
      if (m_filePaths.size() >= kFileLimit) {
        break;
      }
    }
    if (m_thumbnails != nullptr) {
      for (const auto& path : m_filePaths) {
        (void)m_thumbnails->acquire(path);
      }
    }
  }
  m_thumbnailRefreshPending = !m_filePaths.empty();
  if (m_filesGrid != nullptr) {
    m_filesGrid->notifyDataChanged();
  }
  (void)setShown(m_filesEmpty, m_filePaths.empty());
}

void PhoneTab::pullRemoteFile(const std::string& path) {
  if (m_service == nullptr) {
    return;
  }
  const auto dest = m_service->pullFile(path);
  if (!dest) {
    notify::error(
        "Helix", i18n::tr("control-center.phone.title"), i18n::tr("control-center.phone.files-pull-failed")
    );
    return;
  }
  notify::info(
      "Helix", i18n::tr("control-center.phone.title"),
      i18n::tr("control-center.phone.files-pulled", "name", dest->filename().string())
  );
}

void PhoneTab::openContinueItem(const PhoneContinueItem& item) {
  if (item.isFile) {
    const std::string path = localPathFromUrl(item.url);
    if (path.empty() || !process::commandExists("xdg-open")) {
      return;
    }
    (void)process::runAsync(std::vector<std::string>{"xdg-open", path});
    return;
  }
  if (item.url.starts_with("http://") || item.url.starts_with("https://")) {
    (void)net::openInBrowser(item.url);
    return;
  }
  if (m_clipboard != nullptr && !item.url.empty()) {
    (void)m_clipboard->copyText(item.url);
    notify::info(
        "Helix", i18n::tr("control-center.phone.title"), i18n::tr("control-center.phone.continue-copied")
    );
  }
}

void PhoneTab::sendFileTo(const std::string& deviceId) {
  if (m_service == nullptr) {
    return;
  }
  (void)FileDialog::open(
      FileDialogOptions{.title = i18n::tr("control-center.phone.send-file")},
      [this, deviceId](std::optional<std::filesystem::path> path) {
        if (!path || m_service == nullptr) {
          return;
        }
        m_service->sharePath(deviceId, path->string());
      }
  );
}

void PhoneTab::sendClipboardTo(const std::string& deviceId) {
  if (m_service == nullptr || m_clipboard == nullptr) {
    return;
  }
  const auto text = m_clipboard->clipboardText();
  if (!text || text->empty()) {
    return;
  }
  m_service->shareText(deviceId, *text);
}

void PhoneTab::copySmsCode(const std::string& code) {
  if (m_clipboard == nullptr || code.empty()) {
    return;
  }
  (void)m_clipboard->copyText(code);
  notify::info("Helix", i18n::tr("control-center.phone.title"), i18n::tr("control-center.phone.code-copied"));
}

void PhoneTab::dismissPhoneNotification(const PhoneNotification& item) {
  if (m_service == nullptr) {
    return;
  }
  m_service->dismissNotification(item.deviceId, item.publicId);
}

void PhoneTab::syncUi() {
  if (m_status == nullptr || m_syncingUi) {
    return;
  }
  m_syncingUi = true;

  const PhoneSnapshot empty;
  const PhoneSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  const bool available = snap.available;

  const bool layoutDirty = setShown(m_deviceList, available) || setShown(m_activateButton, !available)
      || setShown(m_catalogSwitch, available);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }

  if (!available) {
    m_status->setText(
        snap.activating ? i18n::tr("control-center.phone.starting") : i18n::tr("control-center.phone.stopped")
    );
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.phone.missing") : snap.error);
    if (m_activateButton != nullptr) {
      m_activateButton->setEnabled(m_service != nullptr && !snap.activating);
    }
    m_syncingUi = false;
    return;
  }

  const PhoneDevice* peer = m_service != nullptr ? m_service->peerDevice(m_peerId) : nullptr;
  if (peer != nullptr) {
    if (m_peerId != peer->id) {
      m_peerId = peer->id;
      persistPeer();
    }
    m_status->setText(peer->name.empty() ? peer->id : peer->name);
    m_detail->setText(deviceDetail(*peer));
  } else {
    m_status->setText(
        snap.announcedName.empty() ? i18n::tr("control-center.phone.ready")
                                   : i18n::tr("control-center.phone.visible-as", "name", snap.announcedName)
    );
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.phone.ready-hint") : snap.error);
  }
  rebuildDevices();
  if (m_catalog == Catalog::Files) {
    refreshFiles();
  } else if (m_catalog == Catalog::Notifications) {
    rebuildNotifications();
  } else if (m_catalog == Catalog::Cast) {
    rebuildCast();
  } else {
    rebuildContinue();
  }
  m_syncingUi = false;
}

void PhoneTab::rebuildContinue() {
  if (m_continueList == nullptr || m_service == nullptr) {
    return;
  }
  uiAssertNotRendering("PhoneTab::rebuildContinue");
  const auto& items = m_service->continueItems();
  std::string signature;
  for (const auto& item : items) {
    signature += item.url;
    signature += std::to_string(item.timestamp);
    signature += '|';
  }
  if (signature == m_continueSignature && !m_continueList->children().empty()) {
    (void)setShown(m_continueEmpty, items.empty());
    return;
  }
  m_continueSignature = std::move(signature);
  while (!m_continueList->children().empty()) {
    m_continueList->removeChild(m_continueList->children().front().get());
  }
  const float scale = contentScale();
  (void)setShown(m_continueEmpty, items.empty());
  if (m_continueEmpty != nullptr) {
    m_continueEmpty->setText(i18n::tr("control-center.phone.continue-empty"));
  }
  applyGroupedListStyle(*m_continueList, scale);
  for (const auto& item : items) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .configure = [scale](Flex& host) { applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None); },
    });
    row->addChild(
        ui::glyph({
            .glyph = item.isImage ? "photo" : (item.isFile ? "file" : "link"),
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = item.title.empty() ? item.url : item.title,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    std::string meta = item.deviceName;
    if (item.timestamp > 0) {
      if (!meta.empty()) {
        meta += "  ·  ";
      }
      meta += formatTimeAgo(std::chrono::system_clock::time_point{std::chrono::seconds{item.timestamp}});
    }
    titles->addChild(
        ui::label({
            .text = meta,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    row->addChild(std::move(titles));
    row->addChild(
        ui::button({
            .glyph = "arrow-up-right",
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.phone.continue-open"),
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .onClick = [this, item]() { openContinueItem(item); },
        })
    );
    m_continueList->addChild(std::move(row));
  }
}

void PhoneTab::rebuildNotifications() {
  if (m_notificationsList == nullptr || m_service == nullptr) {
    return;
  }
  uiAssertNotRendering("PhoneTab::rebuildNotifications");
  const std::string peerId = peerDeviceId();
  const auto* peer = m_service->peerDevice(m_peerId);
  std::vector<PhoneNotification> items;
  std::string signature = peerId;
  for (const auto& item : m_service->notifications()) {
    if (!peerId.empty() && item.deviceId != peerId) {
      continue;
    }
    items.push_back(item);
    signature += item.publicId;
    signature += item.title;
    signature += item.smsCode;
    signature += '|';
  }
  if (signature == m_notificationsSignature && !m_notificationsList->children().empty()) {
    (void)setShown(m_notificationsEmpty, items.empty());
    return;
  }
  m_notificationsSignature = std::move(signature);
  while (!m_notificationsList->children().empty()) {
    m_notificationsList->removeChild(m_notificationsList->children().front().get());
  }
  const float scale = contentScale();
  (void)setShown(m_notificationsEmpty, items.empty());
  if (m_notificationsEmpty != nullptr) {
    if (peer != nullptr && !peer->reachable) {
      m_notificationsEmpty->setText(i18n::tr("control-center.phone.notifications-offline"));
    } else if (peer != nullptr && !peer->notificationsPlugin) {
      m_notificationsEmpty->setText(i18n::tr("control-center.phone.notifications-unsupported"));
    } else {
      m_notificationsEmpty->setText(i18n::tr("control-center.phone.notifications-empty"));
    }
  }
  applyGroupedListStyle(*m_notificationsList, scale);
  for (const auto& item : items) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .configure = [scale](Flex& host) { applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None); },
    });
    row->addChild(
        ui::glyph({
            .glyph = item.smsCode.empty() ? "bell" : "key",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    const std::string headline = !item.smsCode.empty()
        ? item.smsCode
        : (!item.title.empty() ? item.title : (!item.ticker.empty() ? item.ticker : item.text));
    titles->addChild(
        ui::label({
            .text = headline,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    std::string meta = item.appName;
    if (!item.smsCode.empty()) {
      if (!meta.empty()) {
        meta += "  ·  ";
      }
      meta += !item.title.empty() ? item.title : item.text;
    } else if (meta.empty()) {
      meta = item.text;
    }
    if (!meta.empty()) {
      titles->addChild(
          ui::label({
              .text = meta,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          })
      );
    }
    row->addChild(std::move(titles));
    if (!item.smsCode.empty()) {
      row->addChild(
          ui::button({
              .glyph = "clipboard",
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.phone.copy-code"),
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .onClick = [this, code = item.smsCode]() { copySmsCode(code); },
          })
      );
    }
    if (item.dismissable) {
      row->addChild(
          ui::button({
              .glyph = "x",
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.phone.notification-dismiss"),
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .onClick = [this, item]() { dismissPhoneNotification(item); },
          })
      );
    }
    m_notificationsList->addChild(std::move(row));
  }
}

void PhoneTab::releaseCastIcons() {
  if (m_thumbnails != nullptr) {
    for (const auto& path : m_castIconPaths) {
      m_thumbnails->release(path, 64);
    }
  }
  m_castIconPaths.clear();
  m_castIconBinds.clear();
}

void PhoneTab::launchCastAppFromUi(const std::string& package, const std::string& label) {
  if (m_service == nullptr || package.empty()) {
    return;
  }
  const std::string peerId = peerDeviceId();
  if (peerId.empty()) {
    return;
  }
  m_service->launchCastApp(peerId, package, label);
  notify::info(
      "Helix", i18n::tr("control-center.phone.title"),
      i18n::tr("control-center.phone.cast-launching", "name", label.empty() ? package : label)
  );
}

void PhoneTab::rebuildCast() {
  if (m_castList == nullptr || m_service == nullptr) {
    return;
  }
  uiAssertNotRendering("PhoneTab::rebuildCast");
  const std::string peerId = peerDeviceId();
  const auto* peer = m_service->peerDevice(m_peerId);
  const auto& cast = m_service->castSnapshot();
  const bool androidPeer = peer != nullptr && m_service->castAndroidFor(*peer);
  const bool applePeer = peer != nullptr && m_service->castAppleFor(*peer);
  const bool supportedPeer = androidPeer || applePeer;
  const bool showAdbRow = androidPeer && cast.toolsReady;
  const bool showMirror = (androidPeer && cast.toolsReady && cast.adbConnected)
      || (applePeer && cast.toolsReady);
  const bool showSearch = androidPeer && showMirror && !cast.apps.empty();
  std::string signature = peerId;
  signature += cast.loading ? '1' : '0';
  signature += cast.connecting ? '1' : '0';
  signature += cast.toolsReady ? '1' : '0';
  signature += cast.adbConnected ? '1' : '0';
  signature += cast.applePeer ? '1' : '0';
  signature += cast.airplayRunning ? '1' : '0';
  signature += cast.receiverAuto ? '1' : '0';
  signature += cast.mirrorStreaming ? '1' : '0';
  signature += cast.pointerRunning ? '1' : '0';
  signature += cast.pointerConnected ? '1' : '0';
  signature += cast.pointerKeyboard ? '1' : '0';
  signature += cast.pointerConfigOk ? '1' : '0';
  signature += cast.pointerBondLost ? '1' : '0';
  signature += cast.error;
  signature += cast.preflightDone ? '1' : '0';
  signature += cast.decoder;
  for (const auto& item : cast.missing) {
    signature += item.label;
  }
  signature += cast.adbSerial;
  signature += cast.suggestedHost;
  signature += cast.airplayName;
  signature += '|';
  signature += m_castQuery;
  for (const auto& app : cast.recent) {
    signature += 'r';
    signature += app.package;
    signature += app.iconPath;
  }
  for (const auto& app : cast.apps) {
    signature += app.package;
    signature += app.label;
    signature += app.iconPath;
    signature += '|';
  }

  auto updateChrome = [&]() {
    if (m_castEmpty != nullptr) {
      if (peer == nullptr) {
        m_castEmpty->setText(i18n::tr("control-center.phone.cast-no-peer"));
      } else if (!supportedPeer) {
        m_castEmpty->setText(i18n::tr("control-center.phone.cast-unsupported"));
      } else if (applePeer) {
        if (!cast.toolsReady) {
          if (!cast.missing.empty()) {
            // One line per missing piece with its fix, so a black window never happens silently.
            std::string text = i18n::tr("control-center.phone.cast-mirror-needs-setup");
            for (const auto& item : cast.missing) {
              text += "\n• " + item.label;
              if (!item.fix.empty()) {
                text += " — " + item.fix;
              }
            }
            m_castEmpty->setText(text);
          } else {
            m_castEmpty->setText(
                cast.error.empty() ? i18n::tr("control-center.phone.cast-airplay-missing") : cast.error
            );
          }
        } else if (cast.airplayRunning && cast.mirrorStreaming) {
          m_castEmpty->setText(i18n::tr("control-center.phone.cast-airplay-streaming"));
        } else if (cast.airplayRunning) {
          m_castEmpty->setText(i18n::tr(
              cast.receiverAuto ? "control-center.phone.cast-airplay-ready" : "control-center.phone.cast-airplay-waiting",
              "name", cast.airplayName.empty() ? "Helix" : cast.airplayName
          ));
        } else {
          m_castEmpty->setText(i18n::tr("control-center.phone.cast-airplay-hint"));
        }
      } else if (cast.connecting) {
        m_castEmpty->setText(i18n::tr("control-center.phone.cast-connecting", "host", cast.suggestedHost));
      } else if (cast.loading) {
        m_castEmpty->setText(i18n::tr("control-center.phone.cast-loading"));
      } else if (!cast.toolsReady) {
        m_castEmpty->setText(i18n::tr("control-center.phone.cast-tools-missing"));
      } else if (!cast.adbConnected) {
        m_castEmpty->setText(cast.error.empty() ? i18n::tr("control-center.phone.cast-adb-missing") : cast.error);
      } else if (cast.apps.empty()) {
        m_castEmpty->setText(i18n::tr("control-center.phone.cast-apps-empty"));
      } else {
        m_castEmpty->setText(
            i18n::tr("control-center.phone.cast-hint-connected", "serial", cast.adbSerial.empty() ? "-" : cast.adbSerial)
        );
      }
      (void)setShown(m_castEmpty, applePeer || !showSearch || cast.apps.empty());
    }
    (void)setShown(m_castAdbRow, showAdbRow);
    (void)setShown(m_castSearchInput, showSearch);
    if (m_castHostInput != nullptr && showAdbRow) {
      const std::string suggested =
          !cast.suggestedHost.empty() ? cast.suggestedHost : m_service->castSuggestedHost(peerId);
      const std::string current = m_castHostInput->value();
      const bool untouched = current.empty() || current == m_castHostPrefill;
      if (untouched && current != suggested) {
        m_castHostInput->setValue(suggested);
      }
      m_castHostPrefill = suggested;
    }
    if (m_castConnectButton != nullptr) {
      m_castConnectButton->setEnabled(showAdbRow && !cast.loading);
      const bool pairing = m_castPairInput != nullptr && !m_castPairInput->value().empty();
      m_castConnectButton->setText(
          pairing ? i18n::tr("control-center.phone.cast-pair")
                  : (cast.adbConnected ? i18n::tr("control-center.phone.cast-reconnect")
                                       : i18n::tr("control-center.phone.cast-connect"))
      );
    }
    if (m_castMirrorButton != nullptr) {
      (void)setShown(m_castMirrorButton, showMirror);
      m_castMirrorButton->setEnabled(showMirror && !cast.loading);
      if (applePeer) {
        // While the phone streams, the button ends that session (and re-arms when always-on).
        m_castMirrorButton->setText(i18n::tr(
            cast.mirrorStreaming  ? "control-center.phone.cast-airplay-disconnect"
            : cast.airplayRunning ? "control-center.phone.cast-airplay-stop"
                                  : "control-center.phone.cast-airplay-start"
        ));
        m_castMirrorButton->setGlyph(cast.airplayRunning ? "player-stop" : "cast");
      } else {
        m_castMirrorButton->setText(i18n::tr("control-center.phone.cast-mirror"));
        m_castMirrorButton->setGlyph("cast");
      }
    }
    (void)setShown(m_castAlwaysOnRow, applePeer && cast.toolsReady);
    if (m_castAlwaysOn != nullptr && m_config != nullptr) {
      m_castAlwaysOn->setChecked(m_config->config().phone.mirrorAlwaysOn);
    }
    const bool showPointer = applePeer && cast.airplayRunning;
    (void)setShown(m_castPointerWell, showPointer);
    if (showPointer) {
      if (m_castPointerStatus != nullptr) {
        const char* key = !cast.pointerRunning    ? "control-center.phone.cast-pointer-starting"
                          : cast.pointerBondLost   ? "control-center.phone.cast-pointer-lost"
                          : !cast.pointerConnected ? "control-center.phone.cast-pointer-waiting"
                          : cast.pointerKeyboard   ? "control-center.phone.cast-pointer-typing"
                                                   : "control-center.phone.cast-pointer-connected";
        m_castPointerStatus->setText(i18n::tr(key));
      }
      if (m_castPointerActions != nullptr) {
        for (const auto& child : m_castPointerActions->children()) {
          if (auto* plate = dynamic_cast<Button*>(child.get())) {
            plate->setEnabled(cast.pointerConnected);
          }
        }
      }
      (void)setShown(m_castPointerSetup, cast.pointerRunning && !cast.pointerConfigOk);
    }
  };

  if (signature == m_castSignature) {
    updateChrome();
    return;
  }
  m_castSignature = std::move(signature);
  releaseCastIcons();
  while (!m_castList->children().empty()) {
    m_castList->removeChild(m_castList->children().front().get());
  }
  const float scale = contentScale();
  updateChrome();
  if (applePeer || cast.apps.empty()) {
    return;
  }

  std::unordered_set<std::string> recentPackages;
  recentPackages.reserve(cast.recent.size());
  for (const auto& app : cast.recent) {
    recentPackages.insert(app.package);
  }

  const std::string query = StringUtils::toLower(m_castQuery);
  applyGroupedListStyle(*m_castList, scale);
  std::size_t shown = 0;
  for (const auto& app : cast.apps) {
    if (!query.empty()) {
      const std::string label = StringUtils::toLower(app.label);
      const std::string package = StringUtils::toLower(app.package);
      if (!label.contains(query) && !package.contains(query)) {
        continue;
      }
    }
    ++shown;
    const bool recent = recentPackages.contains(app.package);
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .configure = [scale](Flex& host) { applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None); },
    });
    auto icon = std::make_unique<CastAppIcon>(scale);
    CastAppIcon* iconPtr = icon.get();
    row->addChild(std::move(icon));
    if (!app.iconPath.empty() && m_thumbnails != nullptr) {
      (void)m_thumbnails->acquire(app.iconPath, 64);
      m_castIconPaths.push_back(app.iconPath);
    }
    m_castIconBinds.push_back(CastIconBind{.icon = iconPtr, .path = app.iconPath, .recent = recent});
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = app.label,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    titles->addChild(
        ui::label({
            .text = recent ? i18n::tr("control-center.phone.cast-recent-meta", "package", app.package) : app.package,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    row->addChild(std::move(titles));
    row->addChild(
        ui::button({
            .glyph = "cast",
            .enabled = cast.adbConnected,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.phone.cast-launch"),
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .onClick = [this, package = app.package, label = app.label]() { launchCastAppFromUi(package, label); },
        })
    );
    m_castList->addChild(std::move(row));
  }
  if (shown == 0 && m_castEmpty != nullptr) {
    m_castEmpty->setText(i18n::tr("control-center.phone.cast-search-empty"));
    (void)setShown(m_castEmpty, true);
  }
  m_thumbnailRefreshPending = m_thumbnailRefreshPending || !m_castIconPaths.empty();
}

void PhoneTab::rebuildDevices() {
  if (m_deviceList == nullptr || m_service == nullptr) {
    return;
  }
  uiAssertNotRendering("PhoneTab::rebuildDevices");
  const auto& snap = m_service->snapshot();
  const PhoneDevice* peer = m_service->peerDevice(m_peerId);
  std::string signature = peer != nullptr ? peer->id : std::string{};
  for (const auto& device : snap.devices) {
    signature += device.id;
    signature += '\n';
    signature += device.name;
    signature += device.reachable ? '1' : '0';
    signature += device.paired ? '1' : '0';
    signature += device.pairRequested ? '1' : '0';
    signature += device.pairRequestedByPeer ? '1' : '0';
    signature += device.verificationKey;
    signature += std::to_string(device.charge);
    signature += device.charging ? '1' : '0';
    signature += '|';
  }
  {
    const auto clip = m_clipboard != nullptr ? m_clipboard->clipboardText() : std::optional<std::string>{};
    signature += (clip.has_value() && !clip->empty()) ? '1' : '0';
  }
  if (signature == m_deviceSignature && !m_deviceList->children().empty()) {
    return;
  }
  m_deviceSignature = std::move(signature);
  while (!m_deviceList->children().empty()) {
    m_deviceList->removeChild(m_deviceList->children().front().get());
  }

  const float scale = contentScale();
  if (snap.devices.empty()) {
    m_deviceList->addChild(
        ui::label({
            .text = i18n::tr("control-center.phone.devices-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    return;
  }

  const float iconSize = Style::controlHeightSm * scale;
  auto iconButton = [scale, iconSize](
                        std::string glyph, bool enabled, std::string tooltip, std::function<void()> onClick
                    ) {
    return ui::button({
        .glyph = std::move(glyph),
        .enabled = enabled,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::move(tooltip),
        .minWidth = iconSize,
        .minHeight = iconSize,
        .padding = Style::spaceXs * scale,
        .onClick = std::move(onClick),
    });
  };

  if (peer != nullptr) {
    auto actions = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
    const bool live = peer->reachable;
    const auto clip = m_clipboard != nullptr ? m_clipboard->clipboardText() : std::optional<std::string>{};
    const bool hasClipboard = clip.has_value() && !clip->empty();
    actions->addChild(iconButton("bell", live, i18n::tr("control-center.phone.ping"), [this, id = peer->id]() {
      if (m_service != nullptr) {
        m_service->ping(id);
      }
    }));
    actions->addChild(iconButton("bell-ringing", live, i18n::tr("control-center.phone.ring"), [this, id = peer->id]() {
      if (m_service != nullptr) {
        m_service->ring(id);
      }
    }));
    actions->addChild(iconButton("paperclip", live, i18n::tr("control-center.phone.send-file"), [this, id = peer->id]() {
      sendFileTo(id);
    }));
    actions->addChild(iconButton(
        "clipboard", live && hasClipboard, i18n::tr("control-center.phone.send-clipboard"),
        [this, id = peer->id]() { sendClipboardTo(id); }
    ));
    if (m_service->castSupportedFor(*peer)) {
      actions->addChild(iconButton("cast", live, i18n::tr("control-center.phone.cast-open"), [this]() {
        applyCatalog(indexFromCatalog(Catalog::Cast));
      }));
    }
    actions->addChild(iconButton("unlink", true, i18n::tr("control-center.phone.unpair"), [this, id = peer->id]() {
      if (m_service != nullptr) {
        m_service->unpair(id);
      }
    }));
    m_deviceList->addChild(std::move(actions));
  }

  std::vector<const PhoneDevice*> otherPaired;
  std::vector<const PhoneDevice*> nearby;
  for (const auto& device : snap.devices) {
    if (peer != nullptr && device.id == peer->id) {
      continue;
    }
    if (device.paired) {
      otherPaired.push_back(&device);
    } else {
      nearby.push_back(&device);
    }
  }

  if (!otherPaired.empty()) {
    auto switcher = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale});
    for (const auto* device : otherPaired) {
      const std::string label = device->name.empty() ? device->id : device->name;
      switcher->addChild(
          ui::button({
              .text = label,
              .glyph = glyphForType(device->type),
              .variant = ButtonVariant::Ghost,
              .onClick = [this, id = device->id]() { selectPeer(id); },
          })
      );
    }
    m_deviceList->addChild(std::move(switcher));
  }

  for (const auto* device : nearby) {
    auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    auto header = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
    header->addChild(
        ui::glyph({
            .glyph = glyphForType(device->type),
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = device->name.empty() ? device->id : device->name,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    titles->addChild(
        ui::label({
            .text = deviceDetail(*device),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    header->addChild(std::move(titles));
    row->addChild(std::move(header));
    if (device->pairRequestedByPeer) {
      auto actions = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
      actions->addChild(
          ui::button({
              .text = i18n::tr("control-center.phone.accept"),
              .glyph = "check",
              .variant = ButtonVariant::Primary,
              .onClick = [this, id = device->id]() {
                if (m_service != nullptr) {
                  m_service->acceptPair(id);
                }
              },
          })
      );
      actions->addChild(
          ui::button({
              .text = i18n::tr("control-center.phone.reject"),
              .glyph = "x",
              .variant = ButtonVariant::Destructive,
              .onClick = [this, id = device->id]() {
                if (m_service != nullptr) {
                  m_service->rejectPair(id);
                }
              },
          })
      );
      row->addChild(std::move(actions));
    } else {
      auto actions = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
      actions->addChild(
          ui::button({
              .text = i18n::tr("control-center.phone.pair"),
              .glyph = "link",
              .enabled = device->reachable,
              .onClick = [this, id = device->id]() {
                if (m_service != nullptr) {
                  m_service->requestPair(id);
                }
              },
          })
      );
      row->addChild(std::move(actions));
    }
    m_deviceList->addChild(std::move(row));
  }
}
