#include "shell/control_center/tabs/type_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/font_family_catalog.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

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

  class FontGridAdapter final : public VirtualGridAdapter {
  public:
    explicit FontGridAdapter(float scale) : m_scale(scale) {
      m_families.push_back({});
      const auto& discovered = settings::discoverFontFamilies();
      m_families.insert(m_families.end(), discovered.begin(), discovered.end());
      m_visible.resize(m_families.size());
      for (std::size_t i = 0; i < m_families.size(); ++i) {
        m_visible[i] = i;
      }
    }

    void setFilter(std::string_view query) {
      const std::string needle = StringUtils::toLower(StringUtils::trim(query));
      m_visible.clear();
      for (std::size_t i = 0; i < m_families.size(); ++i) {
        const std::string& family = m_families[i];
        const std::string label = family.empty() ? StringUtils::toLower(i18n::tr("control-center.appearance.system-font"))
                                                 : StringUtils::toLower(family);
        if (needle.empty() || label.find(needle) != std::string::npos) {
          m_visible.push_back(i);
        }
      }
    }

    [[nodiscard]] std::optional<std::size_t> indexOfFamily(std::string_view family) const {
      for (std::size_t i = 0; i < m_visible.size(); ++i) {
        if (m_families[m_visible[i]] == family) {
          return i;
        }
      }
      return std::nullopt;
    }

    [[nodiscard]] std::string familyAt(std::size_t visibleIndex) const {
      if (visibleIndex >= m_visible.size()) {
        return {};
      }
      return m_families[m_visible[visibleIndex]];
    }

    [[nodiscard]] std::size_t itemCount() const override { return m_visible.size(); }

    [[nodiscard]] std::unique_ptr<Node> createTile() override {
      auto tile = ui::column({
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Center,
          .gap = Style::spaceXs * m_scale,
          .padding = Style::spaceSm * m_scale,
          .radius = Style::scaledRadiusXl(m_scale),
          .border = colorSpecFromRole(ColorRole::Outline, 0.28F),
          .borderWidth = 2.0F,
      });
      tile->addChild(
          ui::label({
              .text = "Aa",
              .fontSize = Style::fontSizeTitle * m_scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          })
      );
      tile->addChild(
          ui::label({
              .fontSize = Style::fontSizeCaption * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          })
      );
      return tile;
    }

    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
      auto* flex = dynamic_cast<Flex*>(&tile);
      if (flex == nullptr || index >= m_visible.size() || flex->children().size() < 2) {
        return;
      }
      const std::string& family = m_families[m_visible[index]];
      auto* specimen = dynamic_cast<Label*>(flex->children()[0].get());
      auto* name = dynamic_cast<Label*>(flex->children()[1].get());
      const ColorSpec fill = selected ? colorSpecFromRole(ColorRole::Primary, 0.18F)
          : hovered                   ? colorSpecFromRole(ColorRole::Hover, 0.55F)
                                      : colorSpecFromRole(ColorRole::SurfaceVariant, 0.55F);
      const ColorSpec border = selected ? colorSpecFromRole(ColorRole::Primary)
                                        : colorSpecFromRole(ColorRole::Outline, 0.28F);
      flex->setFill(fill);
      flex->setBorder(border, 2.0F);
      if (specimen != nullptr) {
        specimen->setFontFamily(family);
        specimen->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
      if (name != nullptr) {
        name->setText(family.empty() ? i18n::tr("control-center.appearance.system-font") : family);
        name->setFontFamily(family);
      }
    }

    [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
      if (index >= m_visible.size()) {
        return {};
      }
      const std::string& family = m_families[m_visible[index]];
      return family.empty() ? i18n::tr("control-center.appearance.system-font") : family;
    }

  private:
    float m_scale = 1.0F;
    std::vector<std::string> m_families;
    std::vector<std::size_t> m_visible;
  };

} // namespace

TypeTab::TypeTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "type", .showConfigureButton = false}
      )) {
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() { m_liveDirty = true; });
  }
}

TypeTab::~TypeTab() = default;

std::unique_ptr<Flex> TypeTab::create() {
  return m_settings->wrapLive(createLiveBody(), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> TypeTab::createLiveBody() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;
  const float controlHeight = Style::controlHeight * scale;

  m_fontAdapter = std::make_unique<FontGridAdapter>(scale);

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto compactCard = ui::column({
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& card) {
            applySectionCardStyle(card, scale, opacity);
            card.setGap(Style::spaceXs * scale);
            card.setPadding(Style::spaceSm * scale);
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
              .placeholder = i18n::tr("control-center.appearance.font-search"),
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
                    refreshFontGrid();
                    PanelManager::instance().requestLayout();
                  },
          })
      )
  );
  toolbar->addChild(
      ui::label({
          .out = &m_fontName,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  compactCard->addChild(std::move(toolbar));
  auto ramp = ui::row({
      .align = FlexAlign::End,
      .gap = Style::spaceMd * scale,
  });
  auto makeTypeSpecimen = [&](Label** labelOut, float fontSize, FontWeight weight, const std::string& caption) {
    auto cell = ui::column({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0F,
    });
    cell->addChild(
        ui::label({
            .out = labelOut,
            .text = "Aa",
            .fontSize = fontSize,
            .fontWeight = weight,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .textAlign = TextAlign::Center,
        })
    );
    cell->addChild(
        ui::label({
            .text = caption,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    return cell;
  };
  ramp->addChild(
      makeTypeSpecimen(
          &m_fontPreviewCaption, Style::fontSizeCaption * scale, FontWeight::Normal,
          i18n::tr("control-center.appearance.specimen-caption")
      )
  );
  ramp->addChild(
      makeTypeSpecimen(
          &m_fontPreviewBody, Style::fontSizeBody * scale, FontWeight::Normal,
          i18n::tr("control-center.appearance.specimen-body")
      )
  );
  ramp->addChild(
      makeTypeSpecimen(
          &m_fontPreview, Style::fontSizeTitle * scale, FontWeight::Medium,
          i18n::tr("control-center.appearance.specimen-title")
      )
  );
  compactCard->addChild(std::move(ramp));
  compactCard->addChild(
      ui::label({
          .out = &m_fontPreviewPangram,
          .text = i18n::tr("control-center.appearance.font-preview"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
      })
  );
  auto chrome = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  auto makeControlSpecimen = [&](Label** labelOut, ColorSpec fill, ColorSpec text, ColorSpec border, float borderWidth,
                                 const std::string& caption) {
    auto cell = ui::column({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0F,
    });
    auto chip = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .paddingH = Style::spaceMd * scale,
        .fill = fill,
        .radius = Style::scaledRadiusMd(scale),
        .border = border,
        .borderWidth = borderWidth,
        .minHeight = controlHeight,
        .fillWidth = true,
    });
    chip->addChild(
        ui::label({
            .out = labelOut,
            .text = "Aa",
            .fontSize = Style::fontSizeBody * scale,
            .color = text,
            .textAlign = TextAlign::Center,
        })
    );
    cell->addChild(std::move(chip));
    cell->addChild(
        ui::label({
            .text = caption,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    return cell;
  };
  chrome->addChild(
      makeControlSpecimen(
          &m_fontPreviewGhost, colorSpecFromRole(ColorRole::SurfaceVariant, 0.55F),
          colorSpecFromRole(ColorRole::OnSurface), colorSpecFromRole(ColorRole::Outline, 0.45F), 1.0F,
          i18n::tr("control-center.appearance.specimen-ghost")
      )
  );
  chrome->addChild(
      makeControlSpecimen(
          &m_fontPreviewPrimary, colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::OnPrimary),
          colorSpecFromRole(ColorRole::Primary), 0.0F, i18n::tr("control-center.appearance.specimen-primary")
      )
  );
  compactCard->addChild(std::move(chrome));
  tab->addChild(std::move(compactCard));

  auto leftover = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  leftover->addChild(
      ui::label({
          .out = &m_empty,
          .text = i18n::tr("control-center.appearance.no-fonts"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  leftover->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .contentScale = scale,
          .columns = 0,
          .minCellWidth = 168.0F * scale,
          .cellHeight = Style::controlHeightLg * 2.15F * scale,
          .columnGap = Style::spaceSm * scale,
          .rowGap = Style::spaceSm * scale,
          .scrollbarVisible = true,
          .adapter = m_fontAdapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::optional<std::size_t> index) {
                if (!index.has_value() || m_syncing || m_fontAdapter == nullptr) {
                  return;
                }
                auto* fonts = static_cast<FontGridAdapter*>(m_fontAdapter.get());
                applyFontFamily(fonts->familyAt(*index));
              },
          .configure =
              [](VirtualGridView& grid) {
                grid.scrollView().clearFill();
                grid.scrollView().clearBorder();
                grid.setFillWidth(true);
              },
      })
  );
  tab->addChild(std::move(leftover));

  refreshFontGrid();
  syncLiveControls();
  return tab;
}

std::unique_ptr<Flex> TypeTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void TypeTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_fontName = nullptr;
  m_fontPreviewGhost = nullptr;
  m_fontPreviewPrimary = nullptr;
  m_fontPreviewCaption = nullptr;
  m_fontPreviewBody = nullptr;
  m_fontPreview = nullptr;
  m_fontPreviewPangram = nullptr;
  m_filter = nullptr;
  m_grid = nullptr;
  m_empty = nullptr;
  m_fontAdapter.reset();
}

void TypeTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool TypeTab::configureMode() const { return m_settings->configureMode(); }

void TypeTab::setSettingsSearchQuery(std::string_view query) {
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
  const std::string& family = m_config->config().shell.fontFamily;
  if (m_fontName != nullptr) {
    m_fontName->setText(family.empty() ? i18n::tr("control-center.appearance.system-font") : family);
    m_fontName->setFontFamily(family);
  }
  const auto applyFamily = [&](Label* label) {
    if (label != nullptr) {
      label->setFontFamily(family);
    }
  };
  applyFamily(m_fontPreviewGhost);
  applyFamily(m_fontPreviewPrimary);
  applyFamily(m_fontPreviewCaption);
  applyFamily(m_fontPreviewBody);
  applyFamily(m_fontPreview);
  applyFamily(m_fontPreviewPangram);
  if (m_grid != nullptr && m_fontAdapter != nullptr) {
    auto* fonts = static_cast<FontGridAdapter*>(m_fontAdapter.get());
    m_grid->setSelectedIndex(fonts->indexOfFamily(family));
  }
  m_syncing = false;
}

void TypeTab::applyFontFamily(std::string family) {
  if (m_syncing || m_config == nullptr) {
    return;
  }
  (void)m_config->setOverride({"shell", "font_family"}, std::move(family));
  syncLiveControls();
}

void TypeTab::refreshFontGrid() {
  auto* fonts = static_cast<FontGridAdapter*>(m_fontAdapter.get());
  if (fonts == nullptr) {
    return;
  }
  fonts->setFilter(m_query);
  const bool empty = fonts->itemCount() == 0;
  setShown(m_empty, empty);
  if (m_grid != nullptr) {
    m_grid->setVisible(!empty);
    m_grid->setParticipatesInLayout(!empty);
    m_grid->notifyDataChanged();
  }
  syncLiveControls();
}
