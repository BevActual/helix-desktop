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
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "type", .showConfigureButton = false}
      )) {
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

  auto typeCard = ui::column({
      .align = FlexAlign::Stretch,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  auto fontRow = makeControlRow(i18n::tr("settings.schema.appearance.font-family.label"), scale, nullptr, false);
  fontRow->addChild(
      ui::button({
          .out = &m_fontButton,
          .text = i18n::tr("control-center.appearance.system-font"),
          .fontSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.appearance.choose-font"),
          .minHeight = controlHeight,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { openFontPicker(); },
      })
  );
  typeCard->addChild(std::move(fontRow));
  typeCard->addChild(
      ui::label({
          .out = &m_fontEmpty,
          .text = i18n::tr("control-center.appearance.no-fonts"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  addWellDivider(*typeCard, scale);
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
  auto chrome = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
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
  typeCard->addChild(std::move(chrome));

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
  typeCard->addChild(std::move(ramp));
  typeCard->addChild(
      ui::label({
          .out = &m_fontPreviewPangram,
          .text = i18n::tr("control-center.appearance.font-preview"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
      })
  );
  addCaption(*typeCard, i18n::tr("settings.schema.appearance.font-family.description"), scale);
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
  m_fontPreviewGhost = nullptr;
  m_fontPreviewPrimary = nullptr;
  m_fontPreviewCaption = nullptr;
  m_fontPreviewBody = nullptr;
  m_fontPreview = nullptr;
  m_fontPreviewPangram = nullptr;
  m_fontEmpty = nullptr;
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
  if (m_config == nullptr) {
    return;
  }
  const std::string& family = m_config->config().shell.fontFamily;
  if (m_fontButton != nullptr) {
    m_fontButton->setText(family.empty() ? i18n::tr("control-center.appearance.system-font") : family);
    if (Label* label = m_fontButton->label(); label != nullptr) {
      label->setFontFamily(family);
    }
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
}

void TypeTab::openFontPicker() {
  if (m_config == nullptr || m_settings == nullptr) {
    return;
  }
  const auto& families = settings::discoverFontFamilies();
  if (families.empty()) {
    if (m_fontEmpty != nullptr) {
      m_fontEmpty->setVisible(true);
      m_fontEmpty->setParticipatesInLayout(true);
    }
    return;
  }
  if (m_fontEmpty != nullptr) {
    m_fontEmpty->setVisible(false);
    m_fontEmpty->setParticipatesInLayout(false);
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
          .placeholder = i18n::tr("control-center.appearance.system-font"),
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
