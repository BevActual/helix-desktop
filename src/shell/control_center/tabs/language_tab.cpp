#include "shell/control_center/tabs/language_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <string>
#include <utility>
#include <vector>

using namespace control_center;

LanguageTab::LanguageTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "language", .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

std::unique_ptr<Flex> LanguageTab::create() {
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
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });

  std::vector<std::string> labels;
  m_languageCodes.clear();
  labels.reserve(i18n::kSupportedLanguages.size() + 1);
  m_languageCodes.reserve(i18n::kSupportedLanguages.size() + 1);
  labels.emplace_back(i18n::tr("common.states.auto"));
  m_languageCodes.emplace_back();
  for (const auto& language : i18n::kSupportedLanguages) {
    labels.emplace_back(language.displayName);
    m_languageCodes.emplace_back(language.code);
  }
  m_language = addSelectRow(
      *card, scale, i18n::tr("settings.schema.appearance.language.label"), std::move(labels), panelCardOpacity(),
      [this](std::size_t index) { applyLanguage(index); }
  );
  addCaption(*card, i18n::tr("settings.schema.appearance.language.description"), scale);
  content->addChild(std::move(card));

  live->addChild(std::move(scroll));
  auto wrapped = m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  syncLiveControls();
  return wrapped;
}

std::unique_ptr<Flex> LanguageTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void LanguageTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_language = nullptr;
  m_languageCodes.clear();
}

void LanguageTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool LanguageTab::configureMode() const { return m_settings->configureMode(); }

void LanguageTab::setSettingsSearchQuery(std::string_view query) {
  if (!query.empty()) {
    m_settings->setConfigureMode(true);
  }
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* LanguageTab::settingsPane() { return m_settings.get(); }

void LanguageTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void LanguageTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_liveDirty) {
    m_liveDirty = false;
    syncLiveControls();
  }
}

void LanguageTab::syncLiveControls() {
  if (m_config == nullptr || m_language == nullptr) {
    return;
  }
  m_syncing = true;
  const std::string& lang = m_config->config().shell.lang;
  std::size_t index = 0;
  for (std::size_t i = 0; i < m_languageCodes.size(); ++i) {
    if (m_languageCodes[i] == lang) {
      index = i;
      break;
    }
  }
  m_language->setSelectedIndexSilently(index);
  m_syncing = false;
}

void LanguageTab::applyLanguage(std::size_t index) {
  if (m_syncing || m_config == nullptr || index >= m_languageCodes.size()) {
    return;
  }
  (void)m_config->setOverride({"shell", "lang"}, m_languageCodes[index]);
}
