#include "shell/control_center/tabs/wallpaper_tab.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_config_overview.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  [[nodiscard]] std::size_t overviewIndex(NiriOverviewPreset preset) {
    switch (preset) {
    case NiriOverviewPreset::Backdrop:
      return 1;
    case NiriOverviewPreset::Stationary:
      return 2;
    case NiriOverviewPreset::Flat:
      return 3;
    case NiriOverviewPreset::None:
    default:
      return 0;
    }
  }

  [[nodiscard]] NiriOverviewPreset overviewFromIndex(std::size_t index) {
    switch (index) {
    case 1:
      return NiriOverviewPreset::Backdrop;
    case 2:
      return NiriOverviewPreset::Stationary;
    case 3:
      return NiriOverviewPreset::Flat;
    default:
      return NiriOverviewPreset::None;
    }
  }

  [[nodiscard]] std::string colorHex(const Color& color) {
    const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

} // namespace

WallpaperTab::WallpaperTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_picker(
          services.platform != nullptr ? &services.platform->wayland() : nullptr, services.config, services.thumbnails,
          services.wallpaperScanner, services.theme, services.httpClient,
          WallpaperPicker::Options{.showTitle = false, .showCloseButton = false}, services.videoWallpaper
      ),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "wallpaper"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> WallpaperTab::create() {
  const float scale = contentScale();
  m_picker.setContentScale(scale);
  m_picker.setPanelCardOpacity(panelCardOpacity());

  auto picker = m_picker.create();
  picker->setFlexGrow(1.0F);

  if (!compositors::isNiri()) {
    return m_settings->wrapLive(std::move(picker), scale, panelCardOpacity());
  }

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto overviewCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*overviewCard, i18n::tr("settings.schema.niri.overview-preset.label"), scale);
  overviewCard->addChild(
      ui::label({
          .text = i18n::tr("settings.schema.niri.overview-preset.description"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  overviewCard->addChild(
      ui::segmented({
          .out = &m_overview,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.options.niri.overview.none")},
                  {.label = i18n::tr("settings.options.niri.overview.backdrop")},
                  {.label = i18n::tr("settings.options.niri.overview.stationary")},
                  {.label = i18n::tr("settings.options.niri.overview.flat")},
              },
          .fontSize = Style::fontSizeCaption * scale,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyOverview(index); },
      })
  );
  live->addChild(std::move(overviewCard));

  m_pickerHost = picker.get();
  live->addChild(std::move(picker));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> WallpaperTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void WallpaperTab::setActive(bool active) {
  if (active) {
    m_picker.onOpen({});
  }
}

void WallpaperTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_picker.onClose();
  m_rootLayout = nullptr;
  m_pickerHost = nullptr;
  m_overview = nullptr;
}

void WallpaperTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool WallpaperTab::configureMode() const { return m_settings->configureMode(); }

void WallpaperTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* WallpaperTab::settingsPane() { return m_settings.get(); }

void WallpaperTab::applyOpenContext(std::string_view context) {
  if (context.empty()) {
    return;
  }
  m_picker.onOpen(context);
}

WallpaperPicker::BrowseSource WallpaperTab::browseSource() const noexcept { return m_picker.browseSource(); }

void WallpaperTab::onPanelCardOpacityChanged(float opacity) { m_picker.setPanelCardOpacity(opacity); }

void WallpaperTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  m_picker.setContentScale(contentScale());
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout != nullptr) {
    m_rootLayout->setSize(contentWidth, bodyHeight);
    m_rootLayout->layout(renderer);
    if (m_pickerHost != nullptr) {
      m_picker.layout(renderer, m_pickerHost->width(), m_pickerHost->height());
    }
    return;
  }
  m_picker.layout(renderer, contentWidth, bodyHeight);
}

void WallpaperTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
  m_picker.update(renderer);
}

void WallpaperTab::syncLiveControls() {
  if (m_config == nullptr || m_overview == nullptr) {
    return;
  }
  m_syncing = true;
  m_overview->setSelectedIndex(overviewIndex(m_config->config().theme.niriLayout.overview));
  m_syncing = false;
}

void WallpaperTab::applyOverview(std::size_t index) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"theme", "niri_layout", "overview"},
      std::string(enumToKey(kNiriOverviewPresets, overviewFromIndex(index)))
  );
  persistOverview();
}

void WallpaperTab::persistOverview() {
  if (m_config == nullptr) {
    return;
  }
  const auto preset = m_config->config().theme.niriLayout.overview;
  if (preset != NiriOverviewPreset::None) {
    const bool wantBackdrop = compositors::niri::overviewWantsBackdrop(preset);
    if (m_config->config().backdrop.enabled != wantBackdrop) {
      (void)m_config->setOverride({"backdrop", "enabled"}, wantBackdrop);
    }
  }
  const auto result = compositors::niri::saveNiriOverview({
      .preset = preset,
      .flatColorHex = colorHex(colorForRole(ColorRole::Surface)),
  });
  if (!result.ok) {
    notify::error(
        "Noctalia", i18n::tr("settings.schema.niri.overview-preset.label"),
        result.error.empty() ? i18n::tr("control-center.theme.niri-save-failed") : result.error
    );
  }
}
