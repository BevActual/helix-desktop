#include "shell/control_center/tabs/desktop_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cstddef>
#include <utility>

using namespace control_center;

namespace {

  [[nodiscard]] std::size_t enabledWidgetCount(const DesktopWidgetsConfig& widgets) {
    std::size_t count = 0;
    for (const auto& widget : widgets.widgets) {
      if (widget.enabled) {
        ++count;
      }
    }
    return count;
  }

} // namespace

DesktopTab::DesktopTab(const ControlCenterServices& services)
    : m_config(services.config), m_widgets(services.desktopWidgets),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "desktop"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> DesktopTab::create() {
  const float scale = contentScale();

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
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  auto addToggleRow = [scale, this](Flex& parent, const std::string& text, Toggle** out, bool checked,
                                    void (DesktopTab::*apply)(bool)) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    row->addChild(
        ui::label({
            .text = text,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    row->addChild(
        ui::toggle({
            .out = out,
            .checkedImmediate = checked,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange =
                [this, apply](bool value) {
                  (this->*apply)(value);
                },
        })
    );
    parent.addChild(std::move(row));
  };

  const Config* cfg = m_config != nullptr ? &m_config->config() : nullptr;
  const bool widgetsEnabled = cfg != nullptr && cfg->desktopWidgets.enabled;
  const bool cornersEnabled = cfg != nullptr && cfg->shell.screenCorners.enabled;
  const bool hotEnabled = cfg != nullptr && cfg->hotCorners.enabled;

  auto widgetsCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*widgetsCard, i18n::tr("settings.navigation.sections.desktop"), scale);
  addToggleRow(
      *widgetsCard, i18n::tr("settings.schema.desktop.widgets.label"), &m_widgetsEnabled, widgetsEnabled,
      &DesktopTab::applyWidgets
  );
  widgetsCard->addChild(
      ui::label({
          .out = &m_widgetCount,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  widgetsCard->addChild(
      ui::button({
          .out = &m_edit,
          .text = i18n::tr("control-center.desktop.edit"),
          .glyph = "pencil",
          .variant = ButtonVariant::Primary,
          .onClick = [this]() { startEdit(); },
      })
  );
  content->addChild(std::move(widgetsCard));

  auto cornersCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*cornersCard, i18n::tr("control-center.desktop.corners"), scale);
  addToggleRow(
      *cornersCard, i18n::tr("settings.schema.desktop.screen-corners-enabled.label"), &m_screenCorners,
      cornersEnabled, &DesktopTab::applyScreenCorners
  );
  addToggleRow(
      *cornersCard, i18n::tr("settings.schema.desktop.hot-corners-enabled.label"), &m_hotCorners,
      hotEnabled, &DesktopTab::applyHotCorners
  );
  content->addChild(std::move(cornersCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> DesktopTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void DesktopTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_widgetsEnabled = nullptr;
  m_screenCorners = nullptr;
  m_hotCorners = nullptr;
  m_widgetCount = nullptr;
  m_edit = nullptr;
}

void DesktopTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool DesktopTab::configureMode() const { return m_settings->configureMode(); }

void DesktopTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* DesktopTab::settingsPane() { return m_settings.get(); }

void DesktopTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void DesktopTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void DesktopTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }

  const Config& cfg = m_config->config();
  m_syncing = true;
  if (m_widgetsEnabled != nullptr) {
    m_widgetsEnabled->setCheckedImmediate(cfg.desktopWidgets.enabled);
  }
  if (m_screenCorners != nullptr) {
    m_screenCorners->setCheckedImmediate(cfg.shell.screenCorners.enabled);
  }
  if (m_hotCorners != nullptr) {
    m_hotCorners->setCheckedImmediate(cfg.hotCorners.enabled);
  }
  if (m_widgetCount != nullptr) {
    m_widgetCount->setText(
        i18n::trp("control-center.desktop.widget-count", static_cast<long>(enabledWidgetCount(cfg.desktopWidgets)))
    );
  }
  m_syncing = false;
}

void DesktopTab::applyWidgets(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"desktop_widgets", "enabled"}, enabled);
  PanelManager::instance().refresh();
}

void DesktopTab::applyScreenCorners(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "screen_corners", "enabled"}, enabled);
  PanelManager::instance().refresh();
}

void DesktopTab::applyHotCorners(bool enabled) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"hot_corners", "enabled"}, enabled);
  PanelManager::instance().refresh();
}

void DesktopTab::startEdit() {
  if (m_config != nullptr && !m_config->config().desktopWidgets.enabled) {
    (void)m_config->setOverride({"desktop_widgets", "enabled"}, true);
  }
  if (m_widgets != nullptr) {
    m_widgets->enterEdit();
  }
}
