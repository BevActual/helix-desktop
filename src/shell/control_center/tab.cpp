#include "shell/control_center/tab.h"

#include "ui/builders.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "render/core/renderer.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace control_center {

  float sectionCardFillOpacity(float panelCardOpacity) noexcept {
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
    // Tab bodies sit as solid wells on the glass panel chrome. Lift the fill so
    // Glass/Soft still read as content, while Solid stays fully opaque.
    card.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, sectionCardFillOpacity(fillOpacity)));
    if (Style::cardBordersEnabled()) {
      card.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
    } else {
      card.clearBorder();
    }
    card.setRadius(Style::scaledRadius2xl(scale));
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Stretch);
    card.setGap(Style::spaceSm * scale);
    card.setPadding(Style::spaceMd * scale);
  }

  Label* addTitle(Flex& parent, const std::string& text, float scale) {
    Label* ptr = nullptr;
    auto label = ui::label({
        .out = &ptr,
        .text = text,
        .fontSize = Style::fontSizeTitle * scale,
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
      std::function<void()> onCommit
  ) {
    auto header = makeControlRow(title, scale, nullptr, false);
    header->addChild(
        ui::label({
            .out = valueOut,
            .text = format(value),
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
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

} // namespace control_center

std::unique_ptr<Flex> Tab::createHeaderActions() { return nullptr; }
