#include "shell/control_center/tab.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace control_center {

  std::unique_ptr<Flex> makeEmptyState(float scale, const EmptyStateSpec& spec, EmptyStateParts* parts) {
    EmptyStateParts local;
    EmptyStateParts& out = parts != nullptr ? *parts : local;
    const float disc = Style::controlHeightLg * 1.4F * scale;
    const float textWidth = 360.0F * scale;

    auto root = ui::column({
        .out = &out.root,
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm * scale,
        .paddingV = Style::spaceLg * scale,
        .paddingH = Style::spaceMd * scale,
        .flexGrow = 1.0F,
    });

    root->addChild(ui::column(
        {
            .align = FlexAlign::Center,
            .justify = FlexJustify::Center,
            .fill = colorSpecFromRole(ColorRole::Primary, 0.14F),
            .radius = disc * 0.5F,
            .minWidth = disc,
            .minHeight = disc,
            .maxWidth = disc,
            .width = disc,
            .height = disc,
        },
        ui::glyph({
            .out = &out.glyph,
            .glyph = spec.glyph,
            .glyphSize = Style::fontSizeHeader * 1.2F * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        })
    ));
    root->addChild(ui::label({
        .out = &out.title,
        .text = spec.title,
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxWidth = textWidth,
        .maxLines = 2,
        .textAlign = TextAlign::Center,
    }));
    root->addChild(ui::label({
        .out = &out.body,
        .text = spec.body,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = textWidth,
        .maxLines = 4,
        .textAlign = TextAlign::Center,
        .visible = !spec.body.empty(),
    }));
    if (!spec.actionLabel.empty()) {
      root->addChild(ui::button({
          .out = &out.action,
          .text = spec.actionLabel,
          .glyph = spec.actionGlyph,
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceLg * scale,
          .radius = Style::scaledRadiusXl(scale),
          .onClick = spec.onAction,
      }));
    }
    return root;
  }

  float sectionCardFillOpacity(float panelCardOpacity) noexcept {
    if (Style::highContrastEnabled()) {
      return 1.0F;
    }
    return panelCardOpacity >= 0.99F ? 1.0F : std::min(1.0F, panelCardOpacity + 0.28F);
  }

  Separator* addWellDivider(Flex& parent, float scale, SeparatorOrientation orientation) {
    Separator* divider = nullptr;
    parent.addChild(ui::separator({
        .out = &divider,
        .color = colorSpecFromRole(ColorRole::Outline, 0.45F),
        .spacing = Style::spaceSm * scale,
        .orientation = orientation,
        .gradientEdges = false,
    }));
    return divider;
  }

  void applySectionCardStyle(Flex& card, float scale, float fillOpacity) {
    // Tab bodies sit as leftover wells on the glass panel chrome. Lift the fill so
    // Glass/Soft still read as content, while Solid stays fully opaque. High Contrast
    // frosts leftover to opaque and keeps a contrasting outline.
    card.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, sectionCardFillOpacity(fillOpacity)));
    if (Style::cardBordersEnabled() || Style::highContrastEnabled()) {
      card.setBorder(
          colorSpecFromRole(ColorRole::Outline),
          Style::highContrastEnabled() ? Style::emphasizedBorderWidth : Style::borderWidth
      );
    } else {
      card.clearBorder();
    }
    card.setRadius(Style::scaledRadius3xl(scale));
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Stretch);
    card.setGap(Style::spaceSm * scale);
    card.setPadding(Style::spaceMd * scale);
  }

  void applyCompactCardStyle(Flex& card, float scale, float fillOpacity, bool hosted) {
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Stretch);
    card.setGap(Style::spaceXs * scale);
    if (hosted) {
      card.clearFill();
      card.clearBorder();
      card.setPadding(0.0F);
      return;
    }
    applySectionCardStyle(card, scale, fillOpacity);
    card.setGap(Style::spaceXs * scale);
    card.setPadding(Style::spaceSm * scale);
  }

  void applyGroupedListStyle(Flex& host, float scale) {
    host.setFill(colorSpecFromRole(ColorRole::Surface));
    if (Style::highContrastEnabled()) {
      host.setBorder(colorSpecFromRole(ColorRole::Outline), Style::emphasizedBorderWidth);
    } else {
      host.clearBorder();
    }
    host.setRadius(Style::scaledRadius2xl(scale));
    host.setGap(0.0F);
    host.setClipChildren(true);
  }

  void applyContentCardStyle(Flex& card, float scale) {
    applyGroupedListStyle(card, scale);
    card.setRadius(Style::scaledRadius3xl(scale));
    card.setPadding(Style::spaceMd * scale);
    card.setGap(Style::spaceSm * scale);
    card.setAlign(FlexAlign::Stretch);
    // A content card stacks header, text, actions. Flex defaults to Horizontal, so a card
    // built as a bare Flex (virtual-list rows) must be told, as applySectionCardStyle does.
    card.setDirection(FlexDirection::Vertical);
  }

  void applyModuleWellStyle(Flex& well, float scale) {
    applyGroupedListStyle(well, scale);
    well.setClipChildren(false);
    well.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, Style::highContrastEnabled() ? 1.0F : 0.84F));
    well.setRadius(Style::scaledRadius3xl(scale));
    well.setPadding(Style::spaceXs * scale);
    well.setGap(0.0F);
    well.setSoftness(Style::highContrastEnabled() ? 0.0F : 1.0F);
    if (!Style::highContrastEnabled()) {
      well.clearBorder();
    }
  }

  std::size_t leftoverCatalogColumns(
      float availableWidth, float minCellWidth, float gap, std::size_t minColumns, std::size_t maxColumns
  ) noexcept {
    minColumns = std::max<std::size_t>(1, minColumns);
    maxColumns = std::max(minColumns, maxColumns);
    if (availableWidth <= 0.0F || minCellWidth <= 0.0F) {
      return minColumns;
    }
    const float cell = minCellWidth + std::max(0.0F, gap);
    const auto fit = static_cast<std::size_t>(std::floor((availableWidth + std::max(0.0F, gap)) / cell));
    return std::clamp(fit, minColumns, maxColumns);
  }

  void applyGroupedListRowStyle(Flex& row, float scale, GroupedListRowEmphasis emphasis) {
    row.setRadius(0.0F);
    row.clearBorder();
    row.setMinHeight(Style::controlHeightLg * scale);
    switch (emphasis) {
    case GroupedListRowEmphasis::Pressed:
      row.setFill(colorSpecFromRole(ColorRole::Primary, 0.18F));
      break;
    case GroupedListRowEmphasis::Hover:
      // A quiet surface tint, like Home pins and settings rows. The palette's Hover role is an
      // opaque accent meant for small buttons; painted across a full list row it reads as a
      // second selection colour.
      row.setFill(colorSpecFromRole(ColorRole::OnSurface, 0.10F));
      break;
    case GroupedListRowEmphasis::Selected:
      row.setFill(colorSpecFromRole(ColorRole::Primary, 0.12F));
      break;
    case GroupedListRowEmphasis::None:
      row.clearFill();
      break;
    }
  }

  Label* addTitle(Flex& parent, const std::string& text, float scale) {
    // Well header. The tab title already sits in the panel header, so wells label their group at
    // body size, the same weight makeCardHeaderRow() uses; only one title size lives inside a tab.
    Label* ptr = nullptr;
    auto label = ui::label({
        .out = &ptr,
        .text = text,
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
    parent.addChild(std::move(label));
    return ptr;
  }

  void addBody(Flex& parent, const std::string& text, float scale) {
    parent.addChild(
        ui::label({
            .text = text,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  std::unique_ptr<Flex> makeCardHeaderRow(const std::string& title, float scale) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
  }

  Label* addCaption(Flex& parent, const std::string& text, float scale, int maxLines) {
    Label* ptr = nullptr;
    parent.addChild(
        ui::label({
            .out = &ptr,
            .text = text,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = maxLines,
        })
    );
    return ptr;
  }

  std::unique_ptr<Flex> makeControlRow(const std::string& title, float scale, Label** titleOut, bool wrap) {
    return ui::row(
        {.align = FlexAlign::Center,
         .wrap = wrap,
         .gap = Style::spaceSm * scale,
         .minHeight = Style::controlHeightSm * scale},
        ui::label({
            .out = titleOut,
            .text = title,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .flexGrow = 1.0F,
        })
    );
  }

  Toggle* addToggleRow(Flex& parent, float scale, const std::string& title, std::function<void(bool)> onChange) {
    Toggle* toggle = nullptr;
    auto row = makeControlRow(title, scale, nullptr, false);
    row->addChild(
        ui::toggle({
            .out = &toggle,
            .checkedImmediate = false,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange = std::move(onChange),
        })
    );
    parent.addChild(std::move(row));
    return toggle;
  }

  Select* addSelectRow(
      Flex& parent, float scale, const std::string& title, std::vector<std::string> options, float surfaceOpacity,
      std::function<void(std::size_t)> onChange
  ) {
    Select* select = nullptr;
    auto row = makeControlRow(title, scale, nullptr, false);
    row->addChild(
        ui::select({
            .out = &select,
            .options = std::move(options),
            .selectedIndex = std::size_t{0},
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .surfaceOpacity = surfaceOpacity,
            .onSelectionChanged =
                [callback = std::move(onChange)](std::size_t index, std::string_view) {
                  if (callback) {
                    callback(index);
                  }
                },
        })
    );
    parent.addChild(std::move(row));
    return select;
  }

  void addSliderSection(
      Flex& parent, float scale, const std::string& title, Slider** sliderOut, Label** valueOut, double minValue,
      double maxValue, double step, double value, std::function<std::string(double)> format,
      std::function<void()> onCommit, std::function<void(double)> onLiveChange
  ) {
    auto header = makeControlRow(title, scale, nullptr, false);
    header->addChild(
        ui::label({
            .out = valueOut,
            .text = format(value),
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
        })
    );
    parent.addChild(std::move(header));
    parent.addChild(
        ui::slider({
            .out = sliderOut,
            .minValue = minValue,
            .maxValue = maxValue,
            .step = step,
            .value = value,
            .trackHeight = Style::sliderTrackHeight * scale,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = Style::controlHeight * scale,
            .flexGrow = 0.0F,
            .onValueChanged =
                [valueOut, format, onLiveChange = std::move(onLiveChange)](double next) {
                  if (valueOut != nullptr && *valueOut != nullptr) {
                    (*valueOut)->setText(format(next));
                  }
                  if (onLiveChange) {
                    onLiveChange(next);
                  }
                },
            .onDragEnd = std::move(onCommit),
            .configure = [](Slider& slider) { slider.setFillWidth(true); },
        })
    );
  }

  void layoutHostedLiveBody(Flex* root, Renderer& renderer, float width) {
    if (root == nullptr) {
      return;
    }
    LayoutConstraints constraints;
    constraints.setExactWidth(std::max(1.0F, width));
    const float height = std::max(1.0F, root->measure(renderer, constraints).height);
    root->setSize(std::max(1.0F, width), height);
    root->layout(renderer);
  }

} // namespace control_center

std::unique_ptr<Flex> Tab::createHeaderActions() { return nullptr; }
