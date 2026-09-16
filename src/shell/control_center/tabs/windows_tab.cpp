#include "shell/control_center/tabs/windows_tab.h"

#include "compositors/niri/niri_layout_theme.h"
#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  [[nodiscard]] std::string pixelLabel(double value) { return std::format("{}px", static_cast<int>(std::lround(value))); }

  [[nodiscard]] std::string percentLabel(double value) {
    return std::format("{:.0f}%", value * 100.0);
  }

  [[nodiscard]] std::string colorHex(const Color& color) {
    const auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0F, 1.0F) * 255.0F); };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

  void addLabeledSlider(
      Flex& card, float scale, float captionSize, float controlHeight, const std::string& title, Slider** sliderOut,
      Label** valueOut, double minValue, double maxValue, double step, double value,
      std::function<std::string(double)> format, std::function<void()> onCommit
  ) {
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });
    header->addChild(
        ui::label({
            .text = title,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .flexGrow = 1.0F,
        })
    );
    header->addChild(
        ui::label({
            .out = valueOut,
            .text = format(value),
            .fontSize = captionSize,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    card.addChild(std::move(header));
    card.addChild(
        ui::slider({
            .out = sliderOut,
            .minValue = minValue,
            .maxValue = maxValue,
            .step = step,
            .value = value,
            .trackHeight = Style::sliderTrackHeight * scale,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = controlHeight,
            .flexGrow = 1.0F,
            .onValueChanged =
                [valueOut, format](double next) {
                  if (valueOut != nullptr && *valueOut != nullptr) {
                    (*valueOut)->setText(format(next));
                  }
                },
            .onDragEnd = std::move(onCommit),
        })
    );
  }

} // namespace

WindowsTab::WindowsTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "windows"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> WindowsTab::create() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float controlHeight = Style::controlHeight * scale;

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });

  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  auto spacingCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*spacingCard, i18n::tr("control-center.windows.spacing-title"), scale);
  spacingCard->addChild(
      ui::label({
          .text = i18n::tr("control-center.windows.spacing-description"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  addLabeledSlider(
      *spacingCard, scale, captionSize, controlHeight,       i18n::tr("settings.schema.niri.gaps.label"), &m_gaps,
      &m_gapsValue, static_cast<double>(noctalia::config::schema::kNiriGapsRange.min.value_or(0)),
      static_cast<double>(noctalia::config::schema::kNiriGapsRange.max.value_or(64)),
      static_cast<double>(noctalia::config::schema::kNiriGapsRange.step.value_or(1)), 16.0, pixelLabel,
      [this]() { commitGaps(); }
  );
  addLabeledSlider(
      *spacingCard, scale, captionSize, controlHeight,       i18n::tr("settings.schema.niri.struts.label"), &m_struts,
      &m_strutsValue, static_cast<double>(noctalia::config::schema::kNiriStrutsRange.min.value_or(0)),
      static_cast<double>(noctalia::config::schema::kNiriStrutsRange.max.value_or(64)),
      static_cast<double>(noctalia::config::schema::kNiriStrutsRange.step.value_or(1)), 0.0, pixelLabel,
      [this]() { commitStruts(); }
  );
  content->addChild(std::move(spacingCard));

  auto columnsCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*columnsCard, i18n::tr("control-center.windows.columns-title"), scale);
  columnsCard->addChild(
      ui::label({
          .text = i18n::tr("control-center.windows.columns-description"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  addLabeledSlider(
      *columnsCard, scale, captionSize, controlHeight, i18n::tr("settings.schema.niri.default-column-width.label"),
      &m_columnWidth, &m_columnWidthValue,       noctalia::config::schema::kNiriColumnWidthRange.min.value_or(0.15),
      noctalia::config::schema::kNiriColumnWidthRange.max.value_or(1.0),
      noctalia::config::schema::kNiriColumnWidthRange.step.value_or(0.05), 0.5, percentLabel,
      [this]() { commitColumnWidth(); }
  );
  content->addChild(std::move(columnsCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> WindowsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void WindowsTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
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

void WindowsTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  m_syncing = true;
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
