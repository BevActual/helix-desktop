#include "shell/control_center/tabs/language_tab.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  Button::ButtonPalette catalogTilePalette() {
    const ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.55F);
    const ColorSpec label = colorSpecFromRole(ColorRole::OnSurface);
    const ColorSpec restBorder = colorSpecFromRole(ColorRole::Outline, 0.28F);
    const ColorSpec selectedBorder = colorSpecFromRole(ColorRole::Primary);
    Button::ButtonStateColors rest{.bg = fill, .border = restBorder, .label = label};
    Button::ButtonStateColors selected{.bg = fill, .border = selectedBorder, .label = label};
    return {
        .borderWidth = 2.0F,
        .normal = rest,
        .hover = selected,
        .pressed = selected,
        .disabled = rest,
        .selected = selected,
    };
  }

  bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    if (changed) {
      node->setVisible(shown);
      node->setParticipatesInLayout(shown);
    }
    return changed;
  }

  [[nodiscard]] std::string_view languageDisplayName(std::string_view code) {
    for (const auto& language : i18n::kSupportedLanguages) {
      if (language.code == code) {
        return language.displayName;
      }
    }
    return {};
  }

  [[nodiscard]] std::string resolvedLanguageName() {
    const std::string_view code = i18n::Service::instance().language();
    const std::string_view name = languageDisplayName(code);
    if (!name.empty()) {
      return std::string(name);
    }
    if (!code.empty()) {
      return std::string(code);
    }
    return "English";
  }

} // namespace

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
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> LanguageTab::createLiveBody(bool hosted) {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity = panelCardOpacity(), hosted](Flex& card) {
            applyCompactCardStyle(card, scale, opacity, hosted);
          },
  });
  auto toolbar = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  toolbar->addChild(
      ui::row(
          {.align = FlexAlign::Center, .flexGrow = 1.0F},
          ui::input({
              .out = &m_filter,
              .placeholder = i18n::tr("control-center.appearance.language-search"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .clearButtonEnabled = true,
              .minLayoutWidth = 128.0F * scale,
              .surfaceOpacity = panelCardOpacity(),
              .height = 0.0F,
              .flexGrow = 1.0F,
              .onChange =
                  [this](const std::string& text) {
                    if (m_query == text) {
                      return;
                    }
                    m_query = text;
                    rebuildLanguageGrid();
                    PanelManager::instance().requestLayout();
                  },
          })
      )
  );
  toolbar->addChild(
      ui::label({
          .out = &m_currentName,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  compactCard->addChild(std::move(toolbar));
  compactCard->addChild(
      ui::label({
          .out = &m_caption,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  tab->addChild(std::move(compactCard));

  auto leftover = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  leftover->addChild(
      ui::label({
          .out = &m_empty,
          .text = i18n::tr("ui.controls.search-picker.empty"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  auto listScroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  auto* content = listScroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  auto grid = std::make_unique<GridView>();
  grid->setColumns(2);
  grid->setColumnGap(Style::spaceSm * scale);
  grid->setRowGap(Style::spaceSm * scale);
  grid->setUniformCellSize(true);
  grid->setStretchItems(true);
  grid->setSquareCells(false);
  grid->setMinCellHeight(Style::controlHeightLg * 1.85F * scale);
  m_grid = grid.get();
  content->addChild(std::move(grid));
  leftover->addChild(std::move(listScroll));
  tab->addChild(std::move(leftover));

  rebuildLanguageGrid();
  syncLiveControls();
  return tab;
}

std::unique_ptr<Flex> LanguageTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void LanguageTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_currentName = nullptr;
  m_caption = nullptr;
  m_filter = nullptr;
  m_grid = nullptr;
  m_empty = nullptr;
  m_tiles.clear();
  m_tileCodes.clear();
  m_lastGridKey.clear();
}

void LanguageTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool LanguageTab::configureMode() const { return m_settings->configureMode(); }

void LanguageTab::setSettingsSearchQuery(std::string_view query) {
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
  if (m_grid != nullptr) {
    const float scale = contentScale();
    const std::size_t columns =
        leftoverCatalogColumns(contentWidth, 148.0F * scale, Style::spaceSm * scale, 2, 5);
    if (m_grid->columns() != columns) {
      m_grid->setColumns(columns);
    }
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
    rebuildLanguageGrid();
    syncLiveControls();
  }
}

void LanguageTab::syncLiveControls() {
  const bool autoLang = m_config == nullptr || m_config->config().shell.lang.empty();
  const std::string selected = autoLang ? std::string{} : m_config->config().shell.lang;
  const std::string_view selectedName = languageDisplayName(selected);
  if (m_currentName != nullptr) {
    if (autoLang) {
      m_currentName->setText(i18n::tr("common.states.auto"));
    } else if (!selectedName.empty()) {
      m_currentName->setText(std::string(selectedName));
    } else {
      m_currentName->setText(selected);
    }
  }
  if (m_caption != nullptr) {
    if (autoLang) {
      m_caption->setText(
          i18n::tr("control-center.appearance.language-auto-caption", "language", resolvedLanguageName())
      );
    } else {
      m_caption->setText(i18n::tr("settings.schema.appearance.language.description"));
    }
  }
  for (std::size_t i = 0; i < m_tiles.size() && i < m_tileCodes.size(); ++i) {
    if (m_tiles[i] != nullptr) {
      m_tiles[i]->setSelected(m_tileCodes[i] == selected);
    }
  }
}

void LanguageTab::rebuildLanguageGrid() {
  if (m_grid == nullptr) {
    return;
  }
  uiAssertNotRendering("LanguageTab::rebuildLanguageGrid");

  struct Entry {
    std::string code;
    std::string label;
    std::string caption;
  };
  std::vector<Entry> entries;
  entries.push_back(Entry{
      .code = {},
      .label = i18n::tr("common.states.auto"),
      .caption = resolvedLanguageName(),
  });
  for (const auto& language : i18n::kSupportedLanguages) {
    entries.push_back(Entry{
        .code = std::string(language.code),
        .label = std::string(language.displayName),
        .caption = std::string(language.code),
    });
  }

  const std::string needle = StringUtils::toLower(StringUtils::trim(m_query));
  if (!needle.empty()) {
    std::erase_if(entries, [&](const Entry& entry) {
      return StringUtils::toLower(entry.label).find(needle) == std::string::npos
          && StringUtils::toLower(entry.caption).find(needle) == std::string::npos
          && StringUtils::toLower(entry.code).find(needle) == std::string::npos;
    });
  }

  const std::string selected = m_config != nullptr ? m_config->config().shell.lang : std::string{};
  std::string gridKey = needle + '|';
  for (const auto& entry : entries) {
    gridKey += entry.code;
    gridKey += ',';
  }
  gridKey += '|';
  gridKey += selected;
  if (gridKey == m_lastGridKey) {
    return;
  }
  m_lastGridKey = std::move(gridKey);

  const bool empty = entries.empty();
  setShown(m_empty, empty);
  if (m_grid != nullptr) {
    m_grid->setVisible(!empty);
    m_grid->setParticipatesInLayout(!empty);
  }
  while (!m_grid->children().empty()) {
    m_grid->removeChild(m_grid->children().back().get());
  }

  m_tiles.clear();
  m_tileCodes.clear();
  m_tiles.reserve(entries.size());
  m_tileCodes.reserve(entries.size());

  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  for (const auto& entry : entries) {
    Button* button = nullptr;
    auto tile = ui::button({
        .out = &button,
        .text = entry.label,
        .fontSize = Style::fontSizeBody * scale,
        .selected = entry.code == selected,
        .contentAlign = ButtonContentAlign::Start,
        .customPalette = catalogTilePalette(),
        .tooltip = entry.label,
        .padding = Style::spaceSm * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusXl(scale),
        .onClick = [this, code = entry.code]() { applyLanguage(code); },
        .configure =
            [captionSize, caption = entry.caption](Button& host) {
              host.setDirection(FlexDirection::Vertical);
              host.setAlign(FlexAlign::Stretch);
              host.setJustify(FlexJustify::Center);
              if (host.label() != nullptr) {
                host.label()->setMaxLines(1);
                host.label()->setEllipsize(TextEllipsize::End);
                host.label()->setFontWeight(FontWeight::Medium);
              }
              host.addChild(
                  ui::label({
                      .text = caption,
                      .fontSize = captionSize,
                      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                      .maxLines = 1,
                      .ellipsize = TextEllipsize::End,
                  })
              );
            },
    });
    m_tiles.push_back(button);
    m_tileCodes.push_back(entry.code);
    m_grid->addChild(std::move(tile));
  }
}

void LanguageTab::applyLanguage(std::string code) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "lang"}, std::move(code));
  rebuildLanguageGrid();
  syncLiveControls();
}
