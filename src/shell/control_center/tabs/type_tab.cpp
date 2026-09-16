#include "shell/control_center/tabs/type_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/settings/font_family_catalog.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <string>
#include <utility>
#include <vector>

using namespace control_center;

TypeTab::TypeTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "type"})) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> TypeTab::create() {
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

  auto addCaption = [scale, captionSize](Flex& parent, const std::string& text) {
    parent.addChild(
        ui::label({
            .text = text,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  };

  auto typeCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  addTitle(*typeCard, i18n::tr("control-center.appearance.type"), scale);

  addCaption(*typeCard, i18n::tr("settings.schema.appearance.font-family.label"));
  typeCard->addChild(
      ui::button({
          .out = &m_fontButton,
          .text = "sans-serif",
          .fontSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Default,
          .minHeight = controlHeight,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .flexGrow = 1.0F,
          .onClick = [this]() { openFontPicker(); },
      })
  );
  content->addChild(std::move(typeCard));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> TypeTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void TypeTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_fontButton = nullptr;
}

void TypeTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool TypeTab::configureMode() const { return m_settings->configureMode(); }

void TypeTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* TypeTab::settingsPane() { return m_settings.get(); }

void TypeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void TypeTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void TypeTab::syncLiveControls() {
  if (m_config == nullptr) {
    return;
  }
  m_syncing = true;
  syncFontButton();
  m_syncing = false;
}

void TypeTab::syncFontButton() {
  if (m_fontButton == nullptr || m_config == nullptr) {
    return;
  }
  const std::string& family = m_config->config().shell.fontFamily;
  m_fontButton->setText(family.empty() ? "sans-serif" : family);
  if (Label* label = m_fontButton->label(); label != nullptr) {
    label->setFontFamily(family);
  }
}

void TypeTab::openFontPicker() {
  if (m_config == nullptr || m_settings == nullptr) {
    return;
  }
  const auto& families = settings::discoverFontFamilies();
  if (families.empty()) {
    return;
  }
  std::vector<settings::SelectOption> options;
  options.reserve(families.size());
  for (const auto& family : families) {
    options.push_back(settings::SelectOption{.value = family, .label = family});
  }
  const std::string selected = m_config->config().shell.fontFamily;
  m_settings->openSearchPicker(
      settings::SearchPickerOpenRequest{
          .title = i18n::tr("settings.schema.appearance.font-family.label"),
          .options = std::move(options),
          .selectedValue = selected,
          .placeholder = "sans-serif",
          .emptyText = i18n::tr("ui.controls.search-picker.empty"),
          .settingPath = {"shell", "font_family"},
          .onSelect =
              [this](const std::string& value) {
                if (m_config != nullptr) {
                  (void)m_config->setOverride({"shell", "font_family"}, value);
                }
                syncFontButton();
              },
      }
  );
}
