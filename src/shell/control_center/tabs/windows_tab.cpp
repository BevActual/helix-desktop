#include "shell/control_center/tabs/windows_tab.h"

#include "compositors/niri/niri_layout_theme.h"
#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] std::string pixelLabel(double value) { return std::format("{}px", static_cast<int>(std::lround(value))); }

  [[nodiscard]] std::string percentLabel(double value) { return std::format("{:.0f}%", value * 100.0); }

  [[nodiscard]] std::string colorHex(const Color& color) {
    const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

} // namespace

std::size_t WindowsTab::indexFromSection(Section section) noexcept {
  return section == Section::Columns ? 1 : 0;
}

WindowsTab::Section WindowsTab::sectionFromIndex(std::size_t index) noexcept {
  return index == 1 ? Section::Columns : Section::Spacing;
}

WindowsTab::Section WindowsTab::sectionFromId(std::string_view id) noexcept {
  return id == "columns" ? Section::Columns : Section::Spacing;
}

std::string_view WindowsTab::idFromSection(Section section) noexcept {
  return section == Section::Columns ? "columns" : "spacing";
}

WindowsTab::WindowsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "windows", .showConfigureButton = false}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> section = m_config->stateString("windows_tab", "section")) {
      m_section = sectionFromId(*section);
    }
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> WindowsTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> WindowsTab::createLiveBody(bool hosted) {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float opacity = panelCardOpacity();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  if (!hosted) {
    auto compactCard = ui::column({
        .configure =
            [scale, opacity, hosted](Flex& card) {
              applyCompactCardStyle(card, scale, opacity, hosted);
            },
    });
    compactCard->addChild(
        ui::segmented({
            .out = &m_sectionSwitch,
            .options =
                std::vector<ui::SegmentedOption>{
                    {
                        .label = "",
                        .glyph = "layout-grid",
                        .tooltip = i18n::tr("control-center.windows.spacing-tooltip"),
                    },
                    {
                        .label = "",
                        .glyph = "layout-board",
                        .tooltip = i18n::tr("control-center.windows.columns-tooltip"),
                    },
                },
            .selectedIndex = indexFromSection(m_section),
            .scale = scale,
            .compact = true,
            .surfaceOpacity = opacity,
            .onChange = [this](std::size_t index) { applySection(index); },
        })
    );
    compactCard->addChild(
        ui::label({
            .out = &m_caption,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    live->addChild(std::move(compactCard));
  }

  const bool spacingOpen = m_section == Section::Spacing;
  auto spacingHost = ui::column({
      .out = &m_spacingHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = spacingOpen,
      .participatesInLayout = spacingOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  addCaption(*spacingHost, i18n::tr("control-center.windows.spacing-description"), scale, 3);
  addSliderSection(
      *spacingHost, scale, i18n::tr("settings.schema.niri.gaps.label"), &m_gaps, &m_gapsValue,
      static_cast<double>(noctalia::config::schema::kNiriGapsRange.min.value_or(0)),
      static_cast<double>(noctalia::config::schema::kNiriGapsRange.max.value_or(64)),
      static_cast<double>(noctalia::config::schema::kNiriGapsRange.step.value_or(1)), 16.0, pixelLabel,
      [this]() { commitGaps(); }
  );
  addSliderSection(
      *spacingHost, scale, i18n::tr("settings.schema.niri.struts.label"), &m_struts, &m_strutsValue,
      static_cast<double>(noctalia::config::schema::kNiriStrutsRange.min.value_or(0)),
      static_cast<double>(noctalia::config::schema::kNiriStrutsRange.max.value_or(64)),
      static_cast<double>(noctalia::config::schema::kNiriStrutsRange.step.value_or(1)), 0.0, pixelLabel,
      [this]() { commitStruts(); }
  );
  live->addChild(std::move(spacingHost));

  const bool columnsOpen = m_section == Section::Columns;
  auto columnsHost = ui::column({
      .out = &m_columnsHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = columnsOpen,
      .participatesInLayout = columnsOpen,
      .configure =
          [scale, hosted](Flex& section) {
            if (!hosted) {
              applyContentCardStyle(section, scale);
            }
          },
  });
  addCaption(*columnsHost, i18n::tr("control-center.windows.columns-description"), scale, 3);
  addSliderSection(
      *columnsHost, scale, i18n::tr("settings.schema.niri.default-column-width.label"), &m_columnWidth,
      &m_columnWidthValue, noctalia::config::schema::kNiriColumnWidthRange.min.value_or(0.15),
      noctalia::config::schema::kNiriColumnWidthRange.max.value_or(1.0),
      noctalia::config::schema::kNiriColumnWidthRange.step.value_or(0.05), 0.5, percentLabel,
      [this]() { commitColumnWidth(); }
  );
  live->addChild(std::move(columnsHost));

  syncLiveControls();
  return live;
}

std::unique_ptr<Flex> WindowsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void WindowsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_sectionSwitch = nullptr;
  m_caption = nullptr;
  m_spacingHost = nullptr;
  m_columnsHost = nullptr;
  m_gaps = nullptr;
  m_gapsValue = nullptr;
  m_struts = nullptr;
  m_strutsValue = nullptr;
  m_columnWidth = nullptr;
  m_columnWidthValue = nullptr;
}

void WindowsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool WindowsTab::configureMode() const { return m_settings->configureMode(); }

void WindowsTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* WindowsTab::settingsPane() { return m_settings.get(); }

void WindowsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void WindowsTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void WindowsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_sectionSwitch != nullptr) {
    m_sectionSwitch->setSurfaceOpacity(opacity);
  }
}

void WindowsTab::syncHosts() {
  const bool spacingOpen = m_section == Section::Spacing;
  const bool layoutDirty = setShown(m_spacingHost, spacingOpen) || setShown(m_columnsHost, !spacingOpen);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void WindowsTab::persistSection() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("windows_tab", "section", std::string(idFromSection(m_section)));
}

void WindowsTab::applySection(std::size_t index) {
  if (m_sectionSyncing) {
    return;
  }
  const Section section = sectionFromIndex(index);
  if (m_section == section) {
    syncHosts();
    return;
  }
  m_section = section;
  persistSection();
  if (m_sectionSwitch != nullptr) {
    m_sectionSyncing = true;
    m_sectionSwitch->setSelectedIndex(indexFromSection(m_section));
    m_sectionSyncing = false;
  }
  syncHosts();
  syncLiveControls();
}

void WindowsTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  m_syncing = true;
  if (m_caption != nullptr) {
    m_caption->setText(
        m_section == Section::Columns ? i18n::tr("control-center.windows.columns-caption")
                                      : i18n::tr("control-center.windows.spacing-caption")
    );
  }
  const auto& niri = m_config->config().theme.niriLayout;
  const double gaps = static_cast<double>(niri.gaps.value_or(16));
  if (m_gaps != nullptr) {
    m_gaps->setValue(gaps);
  }
  if (m_gapsValue != nullptr) {
    m_gapsValue->setText(pixelLabel(gaps));
  }
  const double struts = static_cast<double>(niri.struts.value_or(0));
  if (m_struts != nullptr) {
    m_struts->setValue(struts);
  }
  if (m_strutsValue != nullptr) {
    m_strutsValue->setText(pixelLabel(struts));
  }
  const double columnWidth = niri.defaultColumnWidth.value_or(0.5);
  if (m_columnWidth != nullptr) {
    m_columnWidth->setValue(columnWidth);
  }
  if (m_columnWidthValue != nullptr) {
    m_columnWidthValue->setText(percentLabel(columnWidth));
  }
  syncHosts();
  m_syncing = false;
}

void WindowsTab::commitGaps() {
  if (m_syncing || m_config == nullptr || m_gaps == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "gaps"}, static_cast<std::int64_t>(std::lround(m_gaps->value())));
  persistLayout();
}

void WindowsTab::commitStruts() {
  if (m_syncing || m_config == nullptr || m_struts == nullptr) {
    return;
  }
  (void)m_config->setOverride(
      {"theme", "niri_layout", "struts"}, static_cast<std::int64_t>(std::lround(m_struts->value()))
  );
  persistLayout();
}

void WindowsTab::commitColumnWidth() {
  if (m_syncing || m_config == nullptr || m_columnWidth == nullptr) {
    return;
  }
  (void)m_config->setOverride({"theme", "niri_layout", "default_column_width"}, m_columnWidth->value());
  persistLayout();
}

void WindowsTab::persistLayout() {
  if (m_config == nullptr) {
    return;
  }
  const auto& niriLayout = m_config->config().theme.niriLayout;
  const auto result = compositors::niri::saveNiriLayoutTheme({
      .focusRing = niriLayout.focusRing,
      .border = niriLayout.border,
      .activeColorHex = colorHex(colorForRole(ColorRole::Primary)),
      .inactiveColorHex = colorHex(colorForRole(ColorRole::Outline)),
      .urgentColorHex = colorHex(colorForRole(ColorRole::Error)),
      .tabInactiveColorHex = colorHex(colorForRole(ColorRole::Secondary)),
      .insertHintColorHex = colorHex(colorForRole(ColorRole::Primary)) + "80",
      .gaps = niriLayout.gaps,
      .struts = niriLayout.struts,
      .defaultColumnWidth = niriLayout.defaultColumnWidth,
  });
  if (!result.ok) {
    notify::error(
        "Noctalia", i18n::tr("control-center.windows.save-title"),
        result.error.empty() ? i18n::tr("control-center.windows.save-failed") : result.error
    );
  }
}
